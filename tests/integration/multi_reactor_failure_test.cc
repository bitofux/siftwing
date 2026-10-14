/*
 * PROJECT : SIFTWING
 * FILE    : multi_reactor_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 多Sub部分启动/运行回滚、晚到发布通知屏障与目标线程残留清理
 */
#include "../support/multi_reactor_fixture.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <fcntl.h>
#include <map>

namespace {
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool value, const char *label) {
    require(value, label, checks);
}
std::atomic<int> epoll_calls{0}, event_calls{0}, socket_calls{0};
std::atomic<int> fail_epoll_at{0}, fail_event_at{0}, fail_wake_at{0};
std::atomic<int> first_epoll{-1}, first_wake{-1}, fail_write_fd{-1}, close_error_fd{-1};
std::atomic<int> allocation_mode{0}, watched_recv{-1}, recv_calls{0};
std::atomic<bool> fail_wait{false}, fail_add{false}, fail_mod{false}, pause_wait{false},
    wait_entered{false};
std::atomic<bool> fail_wake_event{false}, fail_thread_allocation{false};
const auto controller_thread = std::this_thread::get_id();
std::atomic<bool> write_entered{false}, write_allowed{false}, inject_stale{false};
std::atomic<std::uint64_t> connection_token{0}, stale_token{0};
std::atomic<unsigned int> stale_epoch{0};
std::atomic<unsigned int> waits{0}, creates{0}, closes{0};
std::atomic<bool> ownership_valid{true};
std::mutex resource_mutex;
std::map<int, std::thread::id> resources;
std::map<int, std::thread::id> pending_sources;
std::atomic<bool> observe_pending{false};
std::atomic<unsigned int> pending_creates{0}, pending_closes{0};
thread_local bool first_sub = false, fail_next_allocation = false, pause_writer = false;
void resource(int fd) {
    if (fd >= 0) {
        const std::lock_guard<std::mutex> lock(resource_mutex);
        resources[fd] = std::this_thread::get_id();
        creates.fetch_add(1);
    }
}
void reset() {
    check(resources.empty() && creates.load() == closes.load() && ownership_valid.load(),
        "all reactor resources closed once on creator thread");
    check(pending_sources.empty() && pending_creates.load() == pending_closes.load(),
        "pending committed sources all closed on target thread");
    observe_pending = false;
    epoll_calls = 0;
    event_calls = 0;
    socket_calls = 0;
    fail_epoll_at = 0;
    fail_event_at = 0;
    fail_thread_allocation = false;
    fail_wake_at = 0;
    first_epoll = -1;
    first_wake = -1;
    fail_write_fd = -1;
    close_error_fd = -1;
    allocation_mode = 0;
    watched_recv = -1;
    recv_calls = 0;
    fail_wait = false;
    fail_add = false;
    fail_mod = false;
    fail_wake_event = false;
    pause_wait = false;
    wait_entered = false;
    write_entered = false;
    write_allowed = false;
    inject_stale = false;
    connection_token = 0;
    stale_token = 0;
    stale_epoch = 0;
    waits = 0;
    first_sub = false;
    fail_next_allocation = false;
}
} // namespace
extern "C" {
int __real_epoll_create1(int);
int __wrap_epoll_create1(int flags) {
    const int ordinal = epoll_calls.fetch_add(1) + 1;
    first_sub = ordinal == 1;
    if (ordinal == fail_epoll_at.load()) {
        errno = EMFILE;
        return -1;
    }
    const int fd = __real_epoll_create1(flags);
    resource(fd);
    if (first_sub)
        first_epoll = fd;
    return fd;
}
int __real_eventfd(unsigned int, int);
int __wrap_eventfd(unsigned int value, int flags) {
    const int ordinal = event_calls.fetch_add(1) + 1;
    if (ordinal == fail_event_at.load()) {
        errno = ENFILE;
        return -1;
    }
    const int fd = __real_eventfd(value, flags);
    resource(fd);
    if (first_sub) {
        first_wake = fd;
        if (allocation_mode.load() == 1)
            fail_next_allocation = true;
    }
    return fd;
}
int __real_socket(int, int, int);
int __wrap_socket(int family, int type, int protocol) {
    socket_calls.fetch_add(1);
    return __real_socket(family, type, protocol);
}
int __real_accept4(int, sockaddr *, socklen_t *, int);
int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags) {
    const int source = __real_accept4(fd, address, size, flags);
    if (source >= 0 && observe_pending.load()) {
        const std::lock_guard<std::mutex> lock(resource_mutex);
        pending_sources[source] = resources.at(first_epoll.load());
        pending_creates.fetch_add(1);
    }
    return source;
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int fd, int operation, int target, epoll_event *event) {
    if (operation == EPOLL_CTL_ADD && event && event->data.u64 == 0
        && event_calls.load() == fail_wake_at.load()) {
        fail_wake_at = 0;
        errno = EIO;
        return -1;
    }
    if (first_sub && event && event->data.u64 > 0) {
        if (operation == EPOLL_CTL_ADD && fail_add.exchange(false)) {
            errno = EIO;
            return -1;
        }
        if (operation == EPOLL_CTL_MOD && fail_mod.exchange(false)) {
            errno = EIO;
            return -1;
        }
        if (operation == EPOLL_CTL_ADD)
            connection_token = event->data.u64;
    }
    return __real_epoll_ctl(fd, operation, target, event);
}
int __real_epoll_wait(int, epoll_event *, int, int);
int __wrap_epoll_wait(int fd, epoll_event *events, int count, int timeout) {
    if (first_sub) {
        const auto epoch = waits.fetch_add(1) + 1;
        wait_entered = true;
        while (pause_wait.load())
            std::this_thread::yield();
        if (fail_wait.exchange(false)) {
            errno = EIO;
            return -1;
        }
        if (fail_wake_event.exchange(false)) {
            events[0] = {};
            events[0].events = EPOLLHUP;
            events[0].data.u64 = 0;
            return 1;
        }
        if (inject_stale.exchange(false)) {
            stale_epoch = epoch;
            events[0] = {};
            events[0].events = EPOLLIN;
            events[0].data.u64 = stale_token.load();
            return 1;
        }
    }
    return __real_epoll_wait(fd, events, count, timeout);
}
ssize_t __real_write(int, const void *, std::size_t);
ssize_t __wrap_write(int fd, const void *bytes, std::size_t size) {
    if (pause_writer) {
        write_entered = true;
        while (!write_allowed.load())
            std::this_thread::yield();
    }
    int expected = fd;
    if (fail_write_fd.compare_exchange_strong(expected, -1)) {
        errno = EIO;
        return -1;
    }
    return __real_write(fd, bytes, size);
}
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __wrap_recv(int fd, void *bytes, std::size_t size, int flags) {
    if (first_sub && fd == watched_recv.load())
        recv_calls.fetch_add(1);
    return __real_recv(fd, bytes, size, flags);
}
int __real_close(int);
int __wrap_close(int fd) {
    {
        const std::lock_guard<std::mutex> lock(resource_mutex);
        const auto pending = pending_sources.find(fd);
        if (pending != pending_sources.end()) {
            if (pending->second != std::this_thread::get_id())
                ownership_valid = false;
            pending_sources.erase(pending);
            pending_closes.fetch_add(1);
        }
        const auto item = resources.find(fd);
        if (item != resources.end()) {
            if (item->second != std::this_thread::get_id())
                ownership_valid = false;
            resources.erase(item);
            closes.fetch_add(1);
        }
    }
    const int result = __real_close(fd);
    int expected = fd;
    if (close_error_fd.compare_exchange_strong(expected, -1)) {
        errno = EINTR;
        return -1;
    }
    return result;
}
void *__real__Znwm(std::size_t);
void *__wrap__Znwm(std::size_t size) {
    // 第一Sub完整ready后，控制者的下一次分配是第二Sub的thread状态；
    // 覆盖线程构造分配失败，不冒称拦截DSO内部pthread_create或真实线程资源耗尽。
    if (std::this_thread::get_id() == controller_thread && epoll_calls.load() == 1
        && event_calls.load() == 1 && fail_thread_allocation.exchange(false))
        throw std::bad_alloc();
    if (fail_next_allocation) {
        fail_next_allocation = false;
        allocation_mode = 0;
        throw std::bad_alloc();
    }
    if (first_sub && size == sizeof(Connection) && allocation_mode.load() == 2) {
        allocation_mode = 0;
        throw std::bad_alloc();
    }
    return __real__Znwm(size);
}
}
int main() {
    const auto baseline = fd_count();
    for (int phase = 0; phase < 5; ++phase) {
        reset();
        if (phase == 0)
            fail_epoll_at = 2;
        else if (phase == 1)
            fail_event_at = 2;
        else if (phase == 2)
            fail_wake_at = 2;
        else if (phase == 3)
            fail_thread_allocation = true;
        else
            allocation_mode = 1;
        MultiReactorFixture server(3);
        const auto error = server.start();
        check(error.value()
                  == (phase == 0   ? EMFILE
                      : phase == 1 ? ENFILE
                      : phase == 2 ? EIO
                      : phase == 3 ? ENOMEM
                                   : ENOMEM),
            "partial startup exact first cause");
        check(
            socket_calls.load() == 0 && server.port() == 0, "failed Sub never opens Main listener");
        check(server.stop() == error, "rollback result retained");
        for (std::size_t i = 0; i < server.size(); ++i)
            check(server.sub(i).status().state != ProtocolSubState::running,
                "all prior ready threads stopped and joined");
    }
    for (int phase = 0; phase < 3; ++phase) {
        reset();
        MultiReactorFixture server(2);
        if (phase == 0)
            fail_epoll_at = 3;
        else if (phase == 1)
            fail_event_at = 3;
        const auto error = phase == 2
                               ? server.start({IpFamily::v4, "invalid numeric host", 0, 16, true})
                               : server.start();
        check(error && server.port() == 0, "Main startup fails after all Sub ready");
        check(server.stop() == error, "Main startup rollback keeps first cause");
        for (std::size_t i = 0; i < server.size(); ++i)
            check(server.sub(i).status().state == ProtocolSubState::joined,
                "Main failure releases all targets");
    }
    for (int phase = 0; phase < 4; ++phase) {
        reset();
        MultiReactorFixture server(2);
        check(!server.start(), "runtime topology ready");
        std::optional<UniqueFd> peer;
        if (phase == 0 || phase == 3) {
            peer = client(server.port());
            await([&] {
                return server.sub(0).status().admitted == 1;
            });
        }
        if (phase == 0) {
            close_error_fd = first_epoll.load();
            fail_wait = true;
        } else if (phase == 1)
            fail_add = true;
        else if (phase == 2)
            allocation_mode = 2;
        else
            fail_wake_event = true;
        if (phase == 1 || phase == 2)
            peer = client(server.port());
        await([&] {
            return server.sub(0).status().first_error.operator bool();
        });
        const auto expected = phase == 2 ? ENOMEM : EIO;
        check(server.sub(0).status().state == ProtocolSubState::quiescing,
            "failed target retains resources before controller barrier");
        check(
            ::fcntl(first_wake.load(), F_GETFD) >= 0, "failed target notification fd still stable");
        check(server.stop().value() == expected, "whole topology propagates Sub original cause");
        const auto status = server.sub(0).status();
        check(status.state == ProtocolSubState::joined && status.active == 0
                  && status.admitted == status.dropped,
            "runtime cleanup complete");
        if (phase == 0)
            check(status.cleanup_error.value() == EINTR && status.first_error.value() == EIO,
                "close diagnostic never masks wait cause or retries");
        else if (phase != 3)
            check(status.received == 1 && status.rejected == 1 && status.admitted == 0,
                "construction/ADD failure owns and closes target source");
    }
    {
        reset();
        ProtocolSubConfig config;
        config.write_byte_budget = 1;
        MultiReactorFixture server(1, config);
        check(!server.start(), "local MOD failure topology");
        fail_mod = true;
        auto peer = client(server.port());
        send_all(peer.get(), wire(1, 77));
        await([&] {
            return server.sub(0).status().dropped == 1;
        });
        check(server.sub(0).status().first_connection_error.value() == EIO
                  && !server.sub(0).status().first_error,
            "client MOD failure stays local");
        auto good = client(server.port());
        send_all(good.get(), wire(1, 78));
        check(
            receive(good.get(), 26) == wire(0x8001, 78), "remaining Sub still serves next client");
        check(!server.stop(), "local error does not fail topology");
    }
    {
        reset();
        pause_wait = true;
        MultiReactorFixture server(1);
        check(!server.start(), "pending queue topology");
        await([] {
            return wait_entered.load();
        });
        observe_pending = true;
        std::vector<UniqueFd> peers;
        for (unsigned int n = 0; n < 32; ++n)
            peers.push_back(client(server.port()));
        await([&] {
            return server.totals().handed_off == 32;
        });
        check(!server.request_stop(), "Main stop while all committed fd remain queued");
        server.stop_main();
        pause_wait = false;
        check(!server.stop(), "stop closes residual queue on Sub");
        const auto status = server.sub(0).status();
        check(status.discarded == 32 && status.received == 0 && status.active == 0,
            "exact pending sources drained without registration");
        check(pending_creates.load() == 32 && pending_closes.load() == 32 && ownership_valid.load(),
            "independent pending close observer proves exactly once on Sub");
    }
    {
        reset();
        pause_wait = true;
        MultiReactorFixture server(1);
        check(!server.start(), "notify failure topology");
        await([] {
            return wait_entered.load();
        });
        fail_write_fd = first_wake.load();
        auto peer = client(server.port());
        await([&] {
            return server.error().operator bool();
        });
        const auto totals = server.totals();
        check(totals.handed_off == 1 && totals.rejected == 0,
            "accepted notification error preserves commit");
        pause_wait = false;
        check(server.stop().value() == EIO, "Main notify cause preserved");
        const auto status = server.sub(0).status();
        check(status.received + status.discarded == 1 && status.admitted == status.dropped,
            "target alone closes committed source");
    }
    // 两个确定性屏障：晚到外部write保持通知fd；内部stop write失败仍以有限wait进入quiescing。
    for (bool fail_internal_stop : {false, true}) {
        reset();
        ProtocolSubReactor sub;
        check(!sub.start(), "direct Sub ready");
        auto target = std::get<MainReactorTarget>(sub.target());
        const int wake = target.wakeup_fd;
        if (fail_internal_stop) {
            fail_write_fd = wake;
            check(sub.request_stop().value() == EIO, "stop write fails but flag remains");
            await([&] {
                return sub.status().state == ProtocolSubState::quiescing;
            });
            check(::fcntl(wake, F_GETFD) >= 0 && !notify_eventfd(wake),
                "last raw control notification still valid");
        } else {
            int pair[2];
            check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
                "owned test stream");
            UniqueFd peer(pair[1]);
            std::atomic<bool> valid{false};
            std::thread producer([&, source = UniqueFd(pair[0])]() mutable {
                pause_writer = true;
                const auto result = try_handoff_and_notify(*target.queue, source, wake);
                pause_writer = false;
                valid = result.handoff == FdHandoffResult::accepted && !result.notification_error
                        && !source;
            });
            await([] {
                return write_entered.load();
            });
            check(!sub.request_stop(), "stop during committed but incomplete publish+notify");
            await([&] {
                return sub.status().state == ProtocolSubState::quiescing;
            });
            check(::fcntl(wake, F_GETFD) >= 0, "wakeup not closed before late publisher returns");
            write_allowed = true;
            producer.join();
            check(valid.load() && !notify_eventfd(wake),
                "publisher and final control notification return before release");
        }
        sub.release_publishers();
        check(!sub.join() && !sub.join(), "join follows explicit publisher barrier");
        check(::fcntl(wake, F_GETFD) == -1 && errno == EBADF && !sub.request_stop(),
            "late internal stop cannot write reused fd");
    }
    {
        reset();
        ProtocolSubReactor sub;
        check(!sub.start(), "stale event target ready");
        auto target = std::get<MainReactorTarget>(sub.target());
        int pair[2];
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
            "first stream");
        UniqueFd source(pair[0]), peer(pair[1]);
        check(try_handoff_and_notify(*target.queue, source, target.wakeup_fd).handoff
                  == FdHandoffResult::accepted,
            "first handoff");
        send_all(peer.get(), wire(1, 60));
        check(receive(peer.get(), 26) == wire(0x8001, 60), "first registration");
        stale_token = connection_token.load();
        (void)peer.close();
        await([&] {
            return sub.status().active == 0;
        });
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
            "replacement stream");
        source = UniqueFd(pair[0]);
        peer = UniqueFd(pair[1]);
        watched_recv = pair[0];
        recv_calls = 0;
        check(try_handoff_and_notify(*target.queue, source, target.wakeup_fd).handoff
                  == FdHandoffResult::accepted,
            "replacement handoff");
        await([&] {
            return sub.status().admitted == 2;
        });
        inject_stale = true;
        await([&] {
            // 下一轮wait证明注入的旧事件已经完成分派，避免过早读取计数。
            return stale_epoch.load() != 0 && waits.load() > stale_epoch.load();
        });
        check(recv_calls.load() == 0 && connection_token.load() != stale_token.load(),
            "stale token never dispatches replacement fd");
        send_all(peer.get(), wire(1, 61));
        check(
            receive(peer.get(), 26) == wire(0x8001, 61), "current token still dispatches normally");
        check(!sub.request_stop() && !notify_eventfd(target.wakeup_fd),
            "all last notifications complete");
        sub.release_publishers();
        check(!sub.join(), "stale event cleanup");
    }
    reset();
    check(fd_count() == baseline, "all failure resources and connections reclaimed");
    std::cout << "multi injection checks=" << checks << '\n';
}
