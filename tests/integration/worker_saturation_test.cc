/*
 * PROJECT : SIFTWING
 * FILE    : worker_saturation_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 正式服务的确定worker队列饱和、连接推进和容量恢复
 */
#include "siftwing/net/online_server.hpp"
#include "siftwing/logging/logger.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include "../support/multi_reactor_fixture.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <nlohmann/json.hpp>

namespace {
using namespace siftwing;
using namespace siftwing::net;
using namespace siftwing::concurrency;
using namespace siftwing::test;
unsigned int checks = 0;
std::atomic<bool> arm{true}, entered{false}, released{false};
std::atomic<unsigned int> accepted{0}, full{0}, preserved{0}, wrong_capacity{0};
std::array<std::atomic<unsigned int>, 32> executions{};
void check(bool value, const char *label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
// 失败展开时先解除测试门，再析构server；不能让生产join等一个已无人释放的worker。
struct ReleaseGate {
    ~ReleaseGate() { released.store(true); }
};
}

// GNU静态链接包装只增加确定同步和观测；调用真实实现，不替换业务结果或队列判定。
query::SearchServiceResult
real_execute(const query::SearchService *, const SearchTask &, query::CacheObservation *, std::optional<std::uint64_t> *) __asm__(
    "__real__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE");
query::SearchServiceResult
wrapped_execute(const query::SearchService *, const SearchTask &, query::CacheObservation *, std::optional<std::uint64_t> *) __asm__(
    "__wrap__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE");
query::SearchServiceResult wrapped_execute(const query::SearchService *service,
    const SearchTask &task,
    query::CacheObservation *observation, std::optional<std::uint64_t> *snapshot_version) {
    const auto id = task.request()->request_id;
    if (id < executions.size()) ++executions[static_cast<std::size_t>(id)];
    if (arm.exchange(false)) {
        entered.store(true);
        while (!released.load()) std::this_thread::yield();
    }
    return real_execute(service, task, observation, snapshot_version);
}
TaskSubmitResult real_submit(
    WorkerPool *, std::unique_ptr<WorkerTask> &) __asm__("__real__ZN8siftwing11concurrency10WorkerPool10try_submitERSt10unique_ptrINS0_10WorkerTaskESt14default_deleteIS3_EE");
TaskSubmitResult wrapped_submit(WorkerPool *, std::unique_ptr<WorkerTask> &)
    __asm__("__wrap__ZN8siftwing11concurrency10WorkerPool10try_submitERSt10unique_ptrINS0_10WorkerTaskESt14default_deleteIS3_EE");
TaskSubmitResult wrapped_submit(
    WorkerPool *pool, std::unique_ptr<WorkerTask> &source) {
    const auto *before = source.get();
    const auto result = real_submit(pool, source);
    if (result == TaskSubmitResult::accepted) {
        if (source) ++wrong_capacity;
        ++accepted;
    } else if (result == TaskSubmitResult::full) {
        const auto state = pool->queue_status();
        if (source.get() == before && before) ++preserved;
        if (state.size != 1 || state.capacity != 1 || state.closed) ++wrong_capacity;
        ++full;
    }
    return result;
}

namespace {
/** @brief 独立读取24字节头与JSON；不使用生产decoder作为响应参考。 */
std::string response(int fd, std::uint16_t type, std::uint64_t id, std::uint16_t status = 0) {
    const auto header = receive(fd, 24);
    std::size_t size = 0;
    for (std::size_t i = 12; i < 16; ++i)
        size = size * 256 + static_cast<unsigned char>(header[i]);
    check(size <= 1024 * 1024, "bounded response length");
    const auto body = receive(fd, size);
    check(header + body == wire(type, id, body, status), "independent response header/type/id/status");
    return body;
}
void eof(int fd) {
    await([&] {
        char byte = 0;
        const auto n = ::recv(fd, &byte, 1, MSG_DONTWAIT);
        if (n == 0) return true;
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return false;
        throw std::runtime_error("unexpected extra bytes or EOF error");
    });
    check(true, "half-closed pipeline ends in EOF");
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
        OnlineServerConfig config;
        config.workers = 1; config.task_capacity = 1; config.sub_count = subs;
        config.sub.write_byte_budget = 7; // 固定错误和PONG都须跨多轮输出，门禁仍串行。
        config.logger = logger.get();
        auto created = OnlineServer::create(std::move(loaded).value(), config);
        if (const auto *error = std::get_if<std::error_code>(&created)) throw std::system_error(*error);
        auto server = std::move(std::get<std::unique_ptr<OnlineServer>>(created));
        ReleaseGate release_on_exit;
        std::size_t connected = 0;
        const auto attach = [&] {
            auto socket = client(server->port());
            ++connected;
            await([&] {
                if (const auto error = server->run_once(0)) throw std::system_error(error);
                return server->report().handed_off == connected;
            });
            return socket;
        };
        auto running = attach();
        send_all(running.get(), wire(3, 11, R"({"query":"unknown"})"));
        await([] { return entered.load() && accepted.load() == 1; });
        auto queued = attach();
        send_all(queued.get(), wire(4, 12, R"({"key":"cat"})"));
        await([] { return accepted.load() == 2; });
        auto rejected = attach();
        // 两种业务均走真实满分支；缓存PING无需新输入即可在错误排空后推进。
        send_all(rejected.get(), wire(3, 13, R"({"query":"red"})") + wire(1, 14));
        check(response(rejected.get(), 0xffff, 13, 2) == R"({"message":"overloaded"})"
                && receive(rejected.get(), 26) == wire(0x8001, 14),
            "full SEARCH then cached PING in wire order");
        send_all(rejected.get(), wire(4, 15, R"({"key":"cat"})") + wire(1, 16));
        check(::shutdown(rejected.get(), SHUT_WR) == 0, "pipeline SHUT_WR");
        check(response(rejected.get(), 0xffff, 15, 2) == R"({"message":"overloaded"})"
                && receive(rejected.get(), 26) == wire(0x8001, 16),
            "full SUGGEST then cached PING through SHUT_WR");
        eof(rejected.get());
        auto healthy = attach();
        auto newcomer = attach();
        for (int round = 0; round < 8; ++round) {
            for (const auto fd : {healthy.get(), newcomer.get()}) {
                send_all(fd, wire(1, 17));
                check(receive(fd, 26) == wire(0x8001, 17), "healthy PING while worker remains gated");
            }
        }
        check(!released.load() && executions[11].load() == 1 && executions[12].load() == 0,
            "running worker still held, queued job has not executed");
        check(full.load() == 2 && preserved.load() == 2 && wrong_capacity.load() == 0,
            "real capacity-one full preserves both task sources");
        check(executions[13].load() == 0 && executions[15].load() == 0,
            "neither rejected job executes");
        await([&] {
            for (const auto &sub : server->report().subs)
                if (sub.requests < 8) return false;
            return true;
        });
        check(!released.load(), "each configured Sub handles traffic before capacity release");
        check(!server->run_once(0) && !server->report().first_error, "overload is local response");
        // 确定门控下核真实execute与排队深度，不用睡眠猜测worker状态。
        await([&] {
            return server->metrics().requests.responses[2] == 2;
        });
        const auto busy = server->metrics();
        check(busy.workers.busy == 1 && busy.queues.tasks == 1 && busy.queues.task_capacity == 1,
            "metrics distinguish busy execute from waiting task");
        released.store(true);
        const auto first = nlohmann::json::parse(response(running.get(), 0x8003, 11));
        check(first.at("results").empty(), "real legal no-match SEARCH completes");
        const auto second = nlohmann::json::parse(response(queued.get(), 0x8004, 12));
        check(second.at("results").at(0).at("term") == "cat", "real queued SUGGEST completes");
        // 完成后复用曾被拒绝的ID是合法新请求；满错误不产生隐式重投或永久熔断。
        send_all(newcomer.get(), wire(3, 13, R"({"query":"unknown"})") + wire(1, 18));
        check(::shutdown(newcomer.get(), SHUT_WR) == 0, "recovered pipeline SHUT_WR");
        const auto recovered = nlohmann::json::parse(response(newcomer.get(), 0x8003, 13));
        check(recovered.at("results").empty(), "capacity release accepts a new request");
        check(receive(newcomer.get(), 26) == wire(0x8001, 18), "recovered pipeline PING");
        eof(newcomer.get());
        check(!server->stop(), "ordinary production stop");
        const auto stable = server->metrics();
        check(stable.workers.busy == 0 && stable.workers.succeeded == 3 && stable.queues.tasks == 0
                  && stable.queries.search.count == 2 && stable.queries.suggest.count == 1
                  && stable.connections.accepted == connected
                  && stable.connections.closed == connected && stable.connections.active == 0
                  && stable.requests.responses[2] == 2 && stable.shutdown.started
                  && stable.shutdown.finished,
            "stable metrics after complete worker/Sub barriers");
        const auto final = server->report();
        check(final.stopped && !final.first_error && final.workers.succeeded == 3
                && final.workers.failed == 0 && accepted.load() == 3 && full.load() == 2,
            "three accepted, two rejected, no retry or worker failure");
        for (std::size_t id = 0; id < executions.size(); ++id)
            check(executions[id].load() == ((id == 11 || id == 12 || id == 13) ? 1U : 0U),
                "exact execution identities after worker join");
        for (const auto &sub : final.subs)
            check(sub.active == 0 && !sub.first_error, "Sub releases all connections without fatal error");
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
        check(events["task_rejected"] == 2, "exact task_rejected hook count");
        std::filesystem::remove_all(log_root);
    }
    check(fd_count() == descriptors, "all server/client descriptors return to baseline");
}
}
int main(int argc, char **argv) {
    try {
        if (argc != 3) throw std::runtime_error("snapshot-parent sub-count required");
        const std::string mode = argv[2];
        if (mode != "1" && mode != "2") throw std::runtime_error("sub-count must be 1 or 2");
        run(argv[1], mode == "1" ? 1U : 2U);
        std::cout << "worker_saturation checks=" << checks << " accepted=" << accepted.load()
                  << " full=" << full.load() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "worker saturation failed: " << error.what() << '\n';
        return 1;
    }
}
