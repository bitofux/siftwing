/*
 * PROJECT : SIFTWING
 * FILE    : online_deadlines_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 真实服务中执行/排队任务到期、晚结果丢弃与新请求恢复
 */
#include "siftwing/net/online_server.hpp"
#include "siftwing/logging/logger.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include "../support/multi_reactor_fixture.hpp"
#include <nlohmann/json.hpp>
namespace {
using namespace siftwing;
using namespace siftwing::net;
using namespace siftwing::concurrency;
using namespace siftwing::test;
std::atomic<bool> first{true}, entered{false}, released{false};
std::atomic<unsigned int> executions{0};
unsigned int checks = 0;
void check(bool ok, const char *why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
// 先释放测试worker门再析构server，避免异常展开把生产join留在无人释放的门上。
struct ReleaseGate { ~ReleaseGate() { released = true; } };
}
query::SearchServiceResult
real_execute(const query::SearchService *, const SearchTask &, query::CacheObservation *) __asm__(
    "__real__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationE");
query::SearchServiceResult
wrapped_execute(const query::SearchService *, const SearchTask &, query::CacheObservation *) __asm__(
    "__wrap__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationE");
query::SearchServiceResult wrapped_execute(const query::SearchService *service,
    const SearchTask &task,
    query::CacheObservation *observation) {
    ++executions;
    if (first.exchange(false)) {
        entered = true;
        while (!released.load()) std::this_thread::yield();
    }
    return real_execute(service, task, observation); // 仍执行正式查询，不伪造结果或更换生产worker。
}
namespace {
void eof(int fd) {
    await([&] {
        char byte;
        const auto result = ::recv(fd, &byte, 1, MSG_DONTWAIT);
        if (result == 0) return true;
        if (result < 0 && (errno == EAGAIN || errno == EINTR)) return false;
        throw std::runtime_error("timeout peer got unexpected byte/error");
    });
}
std::string response(int fd, std::uint16_t type, std::uint64_t id) {
    const auto header = receive(fd, 24);
    std::size_t size = 0;
    for (std::size_t i = 12; i < 16; ++i) size = size * 256 + static_cast<unsigned char>(header[i]);
    check(size <= 1024 * 1024, "response length bound");
    const auto body = receive(fd, size);
    check(header + body == wire(type, id, body), "independent complete response header");
    return body;
}
void run(const std::string &parent, std::size_t subs) {
    const auto descriptors = fd_count();
    {
        char log_pattern[] = "/tmp/siftwing-event-hooks-XXXXXX";
        const char *made = ::mkdtemp(log_pattern);
        check(made != nullptr, "event log fixture directory");
        const std::filesystem::path log_root(made);
        auto logging =
            logging::Logger::create(logging::Level::debug, (log_root / "events").string());
        check(bool(logging), "event logger create");
        auto logger = std::move(logging).value();
        std::cerr << "event fixture=" << log_root << '\n';
        query::SearchServiceConfig resources; resources.parent = parent;
        auto loaded = query::SearchService::create(resources);
        if (!loaded) throw std::runtime_error(loaded.error().context);
        OnlineServerConfig config; config.workers = 1; config.task_capacity = 2; config.sub_count = subs;
        config.sub.request_timeout_ms = 200;
        config.logger = logger.get();
        auto created = OnlineServer::create(std::move(loaded).value(), config);
        if (const auto *error = std::get_if<std::error_code>(&created)) throw std::system_error(*error);
        auto server = std::move(std::get<std::unique_ptr<OnlineServer>>(created));
        ReleaseGate release_on_exit;
        std::size_t connected = 0;
        const auto attach = [&] {
            auto peer = client(server->port()); ++connected;
            await([&] {
                if (const auto error = server->run_once(0)) throw std::system_error(error);
                return server->report().handed_off == connected;
            });
            return peer;
        };
        auto running = attach(); send_all(running.get(), wire(3, 11, R"({"query":"unknown"})"));
        check(::shutdown(running.get(), SHUT_WR) == 0, "half closed running request");
        await([] { return entered.load(); });
        auto queued = attach(); send_all(queued.get(), wire(4, 12, R"({"key":"cat"})"));
        auto healthy = attach();
        send_all(healthy.get(), wire(1, 13));
        check(receive(healthy.get(), 26) == wire(0x8001, 13), "PING progresses with worker held");
        await([&] {
            std::size_t timeouts = 0;
            for (const auto &sub : server->report().subs) timeouts += sub.request_timeouts;
            return timeouts == 2;
        });
        eof(running.get()); eof(queued.get());
        check(!released.load() && executions.load() == 1, "running worker is not cancelled, queued task not yet executed");
        released = true;
        await([&] {
            std::size_t stale = 0;
            for (const auto &sub : server->report().subs) stale += sub.completion_stale;
            return stale == 2;
        });
        check(executions.load() == 2, "both expired jobs still execute exactly once");
        // 同request_id用于新连接，旧结果已用旧generation拒绝，健康请求可重新完成。
        auto retry = attach(); send_all(retry.get(), wire(3, 11, R"({"query":"unknown"})") + wire(1, 14));
        check(::shutdown(retry.get(), SHUT_WR) == 0, "new request pipeline half close");
        check(nlohmann::json::parse(response(retry.get(), 0x8003, 11)).at("results").empty(),
            "new request returns real no-match result");
        check(receive(retry.get(), 26) == wire(0x8001, 14), "new request drains then cached PING");
        eof(retry.get());
        check(!server->stop(), "normal stop after deadline recovery");
        const auto final = server->report();
        check(final.workers.succeeded == 3 && final.workers.failed == 0 && executions.load() == 3,
            "join reports three completed jobs including two expired peers");
        for (const auto &sub : final.subs) {
            check(!sub.first_error && sub.active == 0, "local deadlines do not fail Sub");
            check(sub.completion_received == sub.completion_applied + sub.completion_rejected + sub.completion_stale,
                "completion outcome accounting conserved");
        }
        check(!logger->failed(), "event logger healthy after server stop");
        std::ifstream log_input(log_root / "events");
        std::map<std::string, unsigned> events;
        std::string line;
        while (std::getline(log_input, line)) {
            const auto row = nlohmann::json::parse(line);
            ++events[row.at("event").get<std::string>()];
            check(row.at("index_version") == "0", "actual hook index version");
            if (row.at("event") == "task_rejected")
                check(row.at("code") == "queue_full", "stable full reason");
        }
        check(events["request_expired"] == 2, "exact request_expired hook count");
        check(events["completion_stale"] == 2, "exact completion_stale hook count");
        std::filesystem::remove_all(log_root);
    }
    check(fd_count() == descriptors, "real server and peers release all descriptors");
}
}
int main(int argc, char **argv) {
    try {
        if (argc != 3) throw std::runtime_error("snapshot-parent sub-count required");
        const std::string mode = argv[2];
        if (mode != "1" && mode != "2") throw std::runtime_error("invalid Sub count");
        run(argv[1], mode == "1" ? 1U : 2U);
        std::cout << checks << " online deadline checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << "online deadline failed: " << error.what() << '\n';
        return 1;
    }
}
