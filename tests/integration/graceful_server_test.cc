/*
 * PROJECT : SIFTWING
 * FILE    : graceful_server_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 真实Reactor/Reactor/worker排空、共同截止与晚发布资源屏障
 */
#include "siftwing/net/online_server.hpp"
#include "../support/online_server_fixture.hpp"
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include "siftwing/net/deadline_timer.hpp"
#include <sys/epoll.h>

namespace {
using namespace siftwing;
using namespace siftwing::net;
using namespace siftwing::concurrency;
using namespace siftwing::test;
unsigned int checks = 0;
const auto controller = std::this_thread::get_id();
thread_local bool worker = false;
std::atomic<bool> entered{false}, release_job{false}, pool_closed{false}, main_closed{false};
std::atomic<bool> hold_notify{false}, notify_entered{false}, release_notify{false},
    fail_notify{false};
std::atomic<bool> fail_clock{false};
std::atomic<int> notify_fd{-1};
std::array<std::atomic<unsigned int>, 32> executions{};
std::atomic<std::int64_t> network_deadline{0};
std::atomic<bool> deadline_changed{false}, join_entered{false};
std::atomic<int> listener_fd{-1};
std::atomic<WorkerPool *> pool_ref{nullptr};
void check(bool ok, const char *why) {
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
struct ReleaseGate {
    ~ReleaseGate() {
        release_job = true;
        release_notify = true;
    }
};
}
query::SearchServiceResult real_execute(const query::SearchService *,
    const SearchTask &,
    query::CacheObservation *,
    std::optional<std::uint64_t>
        *) __asm__("__real__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE");
query::SearchServiceResult wrapped_execute(const query::SearchService *,
    const SearchTask &,
    query::CacheObservation *,
    std::optional<std::uint64_t>
        *) __asm__("__wrap__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE");
query::SearchServiceResult wrapped_execute(const query::SearchService *service,
    const SearchTask &task,
    query::CacheObservation *observation,
    std::optional<std::uint64_t> *snapshot_version) {
    worker = true;
    const auto id = task.request()->request_id;
    if (id < executions.size())
        ++executions[static_cast<std::size_t>(id)];
    if (id == 11) {
        entered = true;
        while (!release_job.load())
            std::this_thread::yield();
    }
    return real_execute(service, task, observation, snapshot_version);
}
void real_pool_stop(WorkerPool *) __asm__(
    "__real__ZN8siftwing11concurrency10WorkerPool12request_stopEv");
void wrapped_pool_stop(WorkerPool *) __asm__(
    "__wrap__ZN8siftwing11concurrency10WorkerPool12request_stopEv");
void wrapped_pool_stop(WorkerPool *pool) {
    real_pool_stop(pool);
    pool_ref = pool;
    pool_closed = true;
}
void real_join(WorkerPool *) __asm__("__real__ZN8siftwing11concurrency10WorkerPool4joinEv");
void wrapped_join(WorkerPool *) __asm__("__wrap__ZN8siftwing11concurrency10WorkerPool4joinEv");
void wrapped_join(WorkerPool *pool) {
    join_entered = true; // 由原实现完整join，不用worker统计假冒最后发布已返回。
    real_join(pool);
}
std::error_code real_arm(int, std::optional<DeadlineTime>) noexcept
    __asm__("__real__ZN8siftwing3net18arm_deadline_timerEiSt8optionalIlE");
std::error_code wrapped_arm(int, std::optional<DeadlineTime>) noexcept
    __asm__("__wrap__ZN8siftwing3net18arm_deadline_timerEiSt8optionalIlE");
std::error_code wrapped_arm(int fd, std::optional<DeadlineTime> when) noexcept {
    if (pool_closed.load() && when) {
        std::int64_t empty = 0;
        if (!network_deadline.compare_exchange_strong(empty, *when) && empty != *when)
            deadline_changed = true;
    }
    return real_arm(fd, when);
}
extern "C" {
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int fd, int op, int target, epoll_event *event) {
    // 排空先关闭任务准入，首个DEL为监听；实际close返回后才向观察线程发布关闭事实。
    if (pool_closed.load() && op == EPOLL_CTL_DEL) {
        int empty = -1;
        (void)listener_fd.compare_exchange_strong(empty, target);
    }
    return __real_epoll_ctl(fd, op, target, event);
}
int __real_close(int);
int __wrap_close(int fd) {
    const auto result = __real_close(fd);
    if (fd == listener_fd.load())
        main_closed = true;
    return result;
}
ssize_t __real_write(int, const void *, std::size_t);
ssize_t __wrap_write(int fd, const void *data, std::size_t size) {
    if (worker && size == 8) {
        if (fail_notify.exchange(false)) {
            errno = EIO;
            return -1;
        }
        if (hold_notify.exchange(false)) {
            notify_fd = fd;
            notify_entered = true;
            while (!release_notify.load())
                std::this_thread::yield();
        }
    }
    return __real_write(fd, data, size);
}
int __real_clock_gettime(clockid_t, timespec *);
int __wrap_clock_gettime(clockid_t clock, timespec *value) {
    if (clock == CLOCK_MONOTONIC && std::this_thread::get_id() == controller
        && fail_clock.exchange(false)) {
        errno = EIO;
        return -1;
    }
    return __real_clock_gettime(clock, value);
}
}
namespace {
void reset() {
    entered = false;
    release_job = false;
    pool_closed = false;
    main_closed = false;
    hold_notify = false;
    notify_entered = false;
    release_notify = false;
    fail_notify = false;
    notify_fd = -1;
    network_deadline = 0;
    deadline_changed = false;
    join_entered = false;
    listener_fd = -1;
    pool_ref = nullptr;
    for (auto &value : executions)
        value = 0;
}
std::unique_ptr<OnlineServer> create(const std::string &parent, std::size_t capacity_scale) {
    query::SearchServiceConfig resources;
    resources.parent = parent;
    auto loaded = query::SearchService::create(resources);
    if (!loaded)
        throw std::runtime_error(loaded.error().context);
    OnlineServerConfig config;
    config.connection.max_connections = capacity_scale * 128;
    config.connection.completion_capacity = capacity_scale * 128;
    config.workers = 1;
    config.task_capacity = 2;
    config.connection.write_byte_budget = 7;
    auto created = OnlineServer::create(std::move(loaded).value(), config);
    if (const auto *error = std::get_if<std::error_code>(&created))
        throw std::system_error(*error);
    return std::move(std::get<std::unique_ptr<OnlineServer>>(created));
}
void no_more(int fd) {
    await([&] {
        char byte;
        const auto n = ::recv(fd, &byte, 1, MSG_DONTWAIT);
        if (n == 0 || (n == -1 && errno == ECONNRESET))
            return true;
        if (n == -1 && (errno == EAGAIN || errno == EINTR))
            return false;
        throw std::runtime_error("unexpected additional response");
    });
}
std::string response(int fd, std::uint16_t type, std::uint64_t id) {
    const auto header = receive(fd, 24);
    std::size_t size = 0;
    for (std::size_t n = 12; n < 16; ++n)
        size = size * 256 + static_cast<unsigned char>(header[n]);
    if (size > 1024 * 1024)
        throw std::runtime_error("unbounded response");
    const auto body = receive(fd, size);
    if (header + body != wire(type, id, body))
        throw std::runtime_error("independent response mismatch");
    return body;
}
void scenario(const std::string &parent, std::size_t capacity_scale, int mode) {
    reset();
    const auto descriptors = fd_count();
    {
        auto server = create(parent, capacity_scale);
        siftwing::test::OnlineServerDriver drive(*server);
        ReleaseGate release_on_exit;
        const auto port = server->port();
        std::size_t connected = 0;
        const auto attach_peer = [&] {
            auto peer = client(port);
            ++connected;
            await([&] {
                if (const auto error = server->run_once(0))
                    throw std::system_error(error);
                return server->report().reactor.admitted == connected;
            });
            return peer;
        };
        auto running = attach_peer();
        send_all(running.get(), wire(3, 11, R"({"query":"unknown"})") + wire(1, 13));
        check(::shutdown(running.get(), SHUT_WR) == 0, "pipeline half close");
        await([] {
            return entered.load();
        });
        auto queued = attach_peer();
        send_all(queued.get(), wire(4, 12, R"({"key":"cat"})"));
        auto idle = attach_peer();
        auto partial = attach_peer();
        send_all(partial.get(), wire(1, 14).substr(0, 10));
        await([&] {
            std::size_t requests = 0, active = 0;
            for (const auto &sub : {server->report().reactor}) {
                requests += sub.requests;
                active += sub.active;
            }
            return requests == 2 && active == 4;
        });
        if (mode == 2) {
            hold_notify = true;
            release_job = true;
            await([] {
                return notify_entered.load();
            });
        }
        std::exception_ptr observer_error;
        std::atomic<bool> gate_rejected{false}, listener_closed{false}, targets_retained{false},
            exact_deadline{false};
        std::thread observer([&] {
            ReleaseGate always_release;
            try {
                await([&] {
                    return pool_closed.load() && main_closed.load() && network_deadline.load() > 0;
                });
                exact_deadline = !deadline_changed.load();
                struct Unused final : WorkerTask {
                    void execute() override {
                        throw std::runtime_error("must not execute");
                    }
                };
                std::unique_ptr<WorkerTask> source = std::make_unique<Unused>();
                auto *original = source.get();
                gate_rejected = pool_ref.load()->try_submit(source) == TaskSubmitResult::closed
                                && source.get() == original;
                UniqueFd newcomer(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(port);
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                listener_closed =
                    ::connect(
                        newcomer.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address))
                        == -1
                    && errno == ECONNREFUSED;
                if (mode == 0) {
                    release_job = true;
                    const auto search = nlohmann::json::parse(response(running.get(), 0x8003, 11));
                    const auto suggest = nlohmann::json::parse(response(queued.get(), 0x8004, 12));
                    if (!search.at("results").empty() || suggest.at("command") != "SUGGEST")
                        throw std::runtime_error("real search/suggest result mismatch");
                } else {
                    await([] {
                        return join_entered.load();
                    });
                    if (mode == 1 && (executions[11].load() != 1 || executions[12].load() != 0))
                        throw std::runtime_error(
                            "worker incorrectly cancelled or queued job ran early");
                    if (mode == 2)
                        targets_retained = ::fcntl(notify_fd.load(), F_GETFD) >= 0;
                }
                no_more(idle.get());
                no_more(partial.get());
                if (mode == 0) {
                    no_more(running.get());
                    no_more(queued.get());
                }
            } catch (...) {
                observer_error = std::current_exception();
            }
        });
        const auto error = server->shutdown(mode == 0 ? 5000 : 20);
        observer.join();
        if (observer_error)
            std::rethrow_exception(observer_error);
        check(!error && gate_rejected.load() && listener_closed.load() && exact_deadline.load()
                  && !deadline_changed.load(),
            "new tasks and accept stopped with one common absolute deadline");
        const auto report = server->report();
        std::size_t drained = 0, timed_out = 0, requests = 0;
        for (const auto &sub : {report.reactor}) {
            check(sub.state == ReactorState::stopped && sub.active == 0 && sub.drain_started
                      && !sub.first_error,
                "single owner drained and stopped");
            drained += sub.drain_responses;
            timed_out += sub.drain_timeouts;
            requests += sub.requests;
        }
        check(report.stopped && !server->port() && report.workers.succeeded == 2
                  && !report.workers.failed && executions[11].load() == 1
                  && executions[12].load() == 1 && requests == 2,
            "both accepted jobs executed once, cached request never accepted");
        if (mode == 0)
            check(drained == 2 && timed_out == 0, "both responses drain before deadline");
        if (mode == 1)
            check(drained == 0 && timed_out == 2, "network deadline ends wait before worker join");
        if (mode == 2)
            check(targets_retained.load() && ::fcntl(notify_fd.load(), F_GETFD) == -1
                      && errno == EBADF,
                "complete accepted publication retained fd until final write returned");
        check(!server->shutdown(-1) && !server->stop() && !server->run_once(-1),
            "stopped APIs idempotent, first cause preserved");
    }
    check(fd_count() == descriptors, "all server and peer descriptors released");
}
void errors(const std::string &parent, std::size_t capacity_scale) {
    reset();
    auto server = create(parent, capacity_scale);
    siftwing::test::OnlineServerDriver drive(*server);
    ReleaseGate release_on_exit;
    const auto port = server->port();
    check(server->shutdown(-1).value() == EINVAL && server->shutdown(86400001).value() == EINVAL
              && server->port() == port && !server->report().stopped,
        "invalid timeout preserves healthy server");
    fail_clock = true;
    check(server->shutdown(1000).value() == EIO && !fail_clock.load(),
        "shutdown clock failure uses immediate safe fallback");
    check(server->stop().value() == EIO && server->report().stopped,
        "original clock cause remains stable");
    reset();
    auto empty = create(parent, capacity_scale);
    check(!empty->shutdown(0) && empty->report().stopped,
        "zero duration with no in-flight tasks safely joins");
    reset();
    auto failed = create(parent, capacity_scale);
    siftwing::test::OnlineServerDriver failed_drive(*failed);
    auto peer = client(failed->port());
    send_all(peer.get(), wire(3, 11, R"({"query":"unknown"})"));
    await([&] {
        if (const auto error = failed->run_once(0))
            throw std::system_error(error);
        return entered.load();
    });
    fail_notify = true;
    release_job = true;
    await([&] {
        return bool(failed->report().first_error);
    });
    check(failed->shutdown(1000).value() == EIO && !fail_notify.load(),
        "existing accepted publication failure preserves original cause through shutdown");
    check(failed->report().stopped && failed->report().workers.succeeded == 1,
        "publication failure joins exactly one job without retry");
}
}
int main(int argc, char **argv) {
    try {
        if (argc != 3 || (std::string(argv[2]) != "1" && std::string(argv[2]) != "2"))
            return 2;
        const auto capacity_scale = std::string(argv[2]) == "1" ? 1U : 2U;
        for (int mode = 0; mode != 3; ++mode)
            scenario(argv[1], capacity_scale, mode);
        errors(argv[1], capacity_scale);
        std::cout << checks << " graceful production checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << "graceful production failed: " << error.what() << '\n';
        return 1;
    }
}
