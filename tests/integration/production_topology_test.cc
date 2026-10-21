/*
 * PROJECT : SIFTWING
 * FILE    : production_topology_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-21
 * BRIEF   : 真实OnlineServer的单I/O归属、worker业务、信号优先与控制资源失败回滚
 * IMPLEMENTATION : GNU链接观测不进入生产接口；黄金帧独立编码，业务仍调用真实SearchService。
 */
#include "../support/online_server_fixture.hpp"
#include "siftwing/net/signalfd_source.hpp"
#include <nlohmann/json.hpp>
#include <csignal>
#include <mutex>
#include <set>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>

namespace {
using namespace siftwing;
using namespace siftwing::net;
using namespace siftwing::test;
unsigned checks = 0;
std::thread::id owner;
std::atomic<bool> observe{false}, wrong_thread{false};
std::atomic<unsigned> pollers{0}, timers{0}, notifications{0}, io_calls{0}, executions{0};
std::atomic<int> fail_add{0}, fail_timer{0}, fail_read{0};
std::atomic<bool> signal_on_wait{false}, error_in_signal_batch{false};
std::mutex observations;
std::set<int> connection_fds, control_fds;
std::set<int> unregistered_controls;
unsigned control_closes = 0;
bool close_order = true;
std::set<std::thread::id> business_threads;
void check(bool ok, const char *why) {
    require(ok, why, checks);
}
void check_io_thread() {
    if (observe.load() && std::this_thread::get_id() != owner)
        wrong_thread = true;
}
std::size_t threads() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task")) {
        (void)entry;
        ++count;
    }
    return count;
}
void joined_threads(std::size_t expected) {
    const auto immediate = threads();
    // join已保证发布者代码返回，但Linux任务目录的退出回收可能稍后可见；仍要求有限时间内精确恢复。
    if (immediate != expected)
        std::cout << "post-join task observation: immediate=" << immediate
                  << " expected=" << expected << '\n';
    await([&] {
        return threads() == expected;
    });
    check(threads() == expected, "joined publishers leave no residual task within watchdog");
}
std::unique_ptr<query::SearchService> service(const char *parent) {
    query::SearchServiceConfig config;
    config.parent = parent;
    config.cache.capacity = 2;
    auto result = query::SearchService::create(config);
    if (!result)
        throw std::runtime_error(result.error().context);
    return std::move(result).value();
}
std::unique_ptr<OnlineServer> server(const char *parent, OnlineServerConfig config = {}) {
    auto result = OnlineServer::create(service(parent), config);
    if (auto *error = std::get_if<std::error_code>(&result))
        throw std::system_error(*error);
    return std::move(std::get<std::unique_ptr<OnlineServer>>(result));
}
std::string response(int fd, std::uint16_t type) {
    const auto header = receive(fd, 24);
    check(static_cast<unsigned char>(header[6]) == (type >> 8)
              && static_cast<unsigned char>(header[7]) == (type & 255U),
        "real successful response type");
    std::size_t length = 0;
    for (std::size_t n = 12; n < 16; ++n)
        length = length * 256 + static_cast<unsigned char>(header[n]);
    check(length <= frame_max_body_bytes, "bounded response before allocation");
    return receive(fd, length);
}
void clear_pending() {
    auto result = create_termination_signalfd();
    check(std::holds_alternative<UniqueFd>(result), "temporary drain source");
    auto fd = std::move(std::get<UniqueFd>(result));
    check(drain_termination_signalfd(fd.get()).state == SignalFdDrainState::drained,
        "clear pending signals before restoring mask");
}
} // namespace
using namespace siftwing;
query::SearchServiceResult real_execute(const query::SearchService *,
    const concurrency::SearchTask &,
    query::CacheObservation *,
    std::optional<std::uint64_t>
        *) __asm__("__real__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE");
query::SearchServiceResult wrapped_execute(const query::SearchService *,
    const concurrency::SearchTask &,
    query::CacheObservation *,
    std::optional<std::uint64_t>
        *) __asm__("__wrap__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE");
query::SearchServiceResult wrapped_execute(const query::SearchService *self,
    const concurrency::SearchTask &task,
    query::CacheObservation *cache,
    std::optional<std::uint64_t> *version) {
    if (observe.load()) {
        if (std::this_thread::get_id() == owner)
            wrong_thread = true;
        const std::lock_guard<std::mutex> lock(observations);
        business_threads.insert(std::this_thread::get_id());
        ++executions;
    }
    return real_execute(self, task, cache, version);
}
extern "C" {
int __real_epoll_create1(int);
int __wrap_epoll_create1(int flags) {
    if (observe.load()) {
        ++pollers;
        check_io_thread();
    }
    return __real_epoll_create1(flags);
}
int __real_eventfd(unsigned, int);
int __wrap_eventfd(unsigned value, int flags) {
    if (observe.load()) {
        ++notifications;
        check_io_thread();
    }
    return __real_eventfd(value, flags);
}
int __real_timerfd_create(int, int);
int __wrap_timerfd_create(int clock, int flags) {
    if (fail_timer.exchange(0)) {
        errno = ENFILE;
        return -1;
    }
    if (observe.load()) {
        ++timers;
        check_io_thread();
    }
    return __real_timerfd_create(clock, flags);
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int fd, int operation, int target, epoll_event *event) {
    check_io_thread();
    if (operation == EPOLL_CTL_ADD && fail_add.load() > 0 && --fail_add == 0) {
        errno = ENOSPC;
        return -1;
    }
    const auto result = __real_epoll_ctl(fd, operation, target, event);
    if (observe.load() && !result && operation == EPOLL_CTL_ADD) {
        const std::lock_guard<std::mutex> lock(observations);
        (event->data.u64 ? connection_fds : control_fds).insert(target);
    }
    if (observe.load() && operation == EPOLL_CTL_DEL) {
        const std::lock_guard<std::mutex> lock(observations);
        if (control_fds.count(target))
            unregistered_controls.insert(target);
    }
    return result;
}
int __real_epoll_wait(int, epoll_event *, int, int);
int __wrap_epoll_wait(int fd, epoll_event *events, int count, int timeout) {
    check_io_thread();
    const bool injected = signal_on_wait.exchange(false);
    if (injected && ::kill(::getpid(), SIGTERM))
        std::abort();
    int result = __real_epoll_wait(fd, events, count, timeout);
    if (injected && error_in_signal_batch.load() && result >= 0 && result < count) {
        events[result] = {};
        events[result].events = EPOLLERR;
        events[result++].data.u64 = 0;
    }
    return result;
}
int __real_accept4(int, sockaddr *, socklen_t *, int);
int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags) {
    check_io_thread();
    return __real_accept4(fd, address, size, flags);
}
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __wrap_recv(int fd, void *data, std::size_t size, int flags) {
    if (observe.load()) {
        const std::lock_guard<std::mutex> lock(observations);
        if (connection_fds.count(fd)) {
            ++io_calls;
            check_io_thread();
        }
    }
    return __real_recv(fd, data, size, flags);
}
ssize_t __real_send(int, const void *, std::size_t, int);
ssize_t __wrap_send(int fd, const void *data, std::size_t size, int flags) {
    if (observe.load()) {
        const std::lock_guard<std::mutex> lock(observations);
        if (connection_fds.count(fd)) {
            ++io_calls;
            check_io_thread();
        }
    }
    return __real_send(fd, data, size, flags);
}
ssize_t __real_read(int, void *, std::size_t);
ssize_t __wrap_read(int fd, void *data, std::size_t size) {
    if (fail_read.exchange(0)) {
        errno = EIO;
        return -1;
    }
    return __real_read(fd, data, size);
}
int __real_close(int);
int __wrap_close(int fd) {
    if (observe.load()) {
        const std::lock_guard<std::mutex> lock(observations);
        if (connection_fds.count(fd) || control_fds.count(fd))
            check_io_thread();
        if (control_fds.count(fd)) {
            ++control_closes;
            close_order = close_order && unregistered_controls.count(fd) != 0;
        }
    }
    return __real_close(fd);
}
}
int main(int argc, char **argv) {
    try {
        if (argc != 3)
            return 2;
        owner = std::this_thread::get_id();
        sigset_t original;
        check(::pthread_sigmask(SIG_BLOCK, nullptr, &original) == 0 && !block_termination_signals(),
            "block before all worker creation");
        // 首次启动线程会惰性建立TSan辅助线程；预热后仍严格核对生产新增两个worker。
        // join与/proc任务条目回收不是同一观察点，先等空线程条目消失再取资源基线。
        long warmup_tid = 0;
        std::thread runtime_warmup([&] {
            warmup_tid = ::syscall(SYS_gettid);
        });
        runtime_warmup.join();
        await([&] {
            return !std::filesystem::exists("/proc/self/task/" + std::to_string(warmup_tid));
        });
        const auto descriptors = fd_count(), original_threads = threads();
        OnlineServerConfig config;
        config.workers = 2;
        config.reactor.termination_signals = true;
        {
            auto ready = service(argv[1]);
            observe = true;
            auto made = OnlineServer::create(std::move(ready), config);
            check(std::holds_alternative<std::unique_ptr<OnlineServer>>(made), "production ready");
            auto running = std::move(std::get<std::unique_ptr<OnlineServer>>(made));
            check(threads() == original_threads + 2 && pollers == 1 && timers == 1
                      && notifications == 1,
                "exactly one epoll/timer/eventfd and only the two worker threads");
            auto peer = client(running->port());
            OnlineServerDriver drive(*running);
            for (std::uint64_t id = 1; id <= 6; ++id) {
                const bool search = id % 2 != 0;
                send_all(peer.get(),
                    wire(search ? 3 : 4,
                        id,
                        search ? "{\"query\":\"world\",\"top_k\":2}"
                               : "{\"key\":\"wor\",\"top_k\":2}"));
                const auto answer =
                    nlohmann::json::parse(response(peer.get(), search ? 0x8003 : 0x8004));
                check(answer.at("command") == (search ? "SEARCH" : "SUGGEST"),
                    "real service not a PING-only probe");
            }
            check(!running->shutdown(1000), "same-thread network drain then full worker join");
            drive.release();
            const auto metrics = running->metrics();
            check(executions == 6 && !business_threads.empty() && business_threads.size() <= 2
                      && io_calls > 0 && !wrong_thread && metrics.queries.search.count == 3
                      && metrics.queries.suggest.count == 3,
                "every actual business execution is on worker; all observed connection and polling I/O on owner");
            joined_threads(original_threads);
            check(running->report().reactor.state == ReactorState::stopped,
                "full publisher stop without an I/O thread join");
            check(control_fds.size() == 4 && control_closes == 4 && close_order,
                "unique listener/eventfd/timerfd/signalfd each DEL before one owner-thread close");
            observe = false;
            running.reset();
        }
        check(fd_count() == descriptors, "production topology returns all descriptors");
        for (int position = 1; position <= 4; ++position) {
            auto ready = service(argv[1]);
            fail_add = position;
            const auto made = OnlineServer::create(std::move(ready), config);
            fail_add = 0;
            const auto *error = std::get_if<std::error_code>(&made);
            joined_threads(original_threads);
            check(error && error->value() == ENOSPC && fd_count() == descriptors,
                "all four control/listener ADD startup failures reclaim real resources and workers");
        }
        {
            auto ready = service(argv[1]);
            fail_timer = 1;
            const auto made = OnlineServer::create(std::move(ready), config);
            check(std::get<std::error_code>(made).value() == ENFILE && fd_count() == descriptors,
                "single production timer creation rollback");
        }
        for (const int signo : {SIGTERM, SIGINT}) {
            auto running = server(argv[1], config);
            auto peer = client(running->port());
            check(::kill(::getpid(), signo) == 0 && !running->run_once(100)
                      && running->shutdown_signal() == signo && running->report().accepted == 0,
                "real ready signal precedes listener admission");
            check(!running->run_once(0) && running->report().accepted == 0 && !running->shutdown(0),
                "repeated signal observation admits no new connections");
            running.reset();
            clear_pending();
        }
        {
            auto running = server(argv[1], config);
            signal_on_wait = true;
            error_in_signal_batch = true;
            check(running->run_once(100).value() == EIO && !running->shutdown_signal()
                      && running->report().stopped,
                "same batch control I/O error outranks graceful signal");
            error_in_signal_batch = false;
            running.reset();
            clear_pending();
        }
        {
            auto running = server(argv[1], config);
            fail_read = 1;
            check(running->run_once(0).value() == EIO && running->report().stopped,
                "control read failure stops production with original cause");
        }
        clear_pending();
        joined_threads(original_threads);
        check(fd_count() == descriptors, "failure suites leave no fd or thread");
        check(::pthread_sigmask(SIG_SETMASK, &original, nullptr) == 0,
            "restore test mask only after complete stop");
        std::cout << checks << " production topology checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
