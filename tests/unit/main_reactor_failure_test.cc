/*
 * PROJECT : SIFTWING
 * FILE    : main_reactor_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 创建事务、接入资源错误、通知失败归属与一次清理诊断
 */
#include "siftwing/net/main_reactor.hpp"
#include <arpa/inet.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <netinet/tcp.h>
#include <new>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
thread_local int socket_error = 0, epoll_error = 0, event_error = 0, ctl_error_at = -1, ctl_calls = 0;
thread_local int wait_error = 0, accept_error = 0, accept_after = -1, accept_until = -1, accept_calls = 0, configure_error = 0;
thread_local int watched_write = -1, write_error = 0, watched_close = -1, close_error = 0, close_calls = 0;
thread_local int lock_error_at = -1, lock_calls = 0, allocation_error_at = -1, allocation_calls = 0;
thread_local int observed_accepted = -1, total_creates = 0;
thread_local FdHandoffQueue *close_on_accept = nullptr;
thread_local std::thread::id closed_in;
void require(bool value, const char *label) {
    ++checks; if (!value) { std::cerr << "Main injection failed: " << label << '\n'; std::exit(1); }
}
std::size_t fd_count() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++count; }
    return count;
}
UniqueFd notification() {
    auto result = create_eventfd_wakeup(); require(std::holds_alternative<UniqueFd>(result), "real eventfd");
    return std::move(std::get<UniqueFd>(result));
}
std::unique_ptr<MainReactor> create(const std::vector<MainReactorTarget> &targets, bool configure = false, std::size_t budget = 4) {
    MainReactorConfig config; config.tcp_no_delay = configure; config.accept_attempt_budget = budget;
    auto result = MainReactor::create(targets, {}, config); require(std::holds_alternative<std::unique_ptr<MainReactor>>(result), "complete Main");
    return std::move(std::get<std::unique_ptr<MainReactor>>(result));
}
UniqueFd client(std::uint16_t port) {
    UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)); require(static_cast<bool>(fd), "client");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    require(::connect(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "real connect"); return fd;
}
} // namespace
extern "C" {
int __real_socket(int,int,int);
int __wrap_socket(int domain, int type, int protocol) {
    ++total_creates; if (socket_error) { errno = socket_error; return -1; } return __real_socket(domain,type,protocol);
}
int __real_epoll_create1(int);
int __wrap_epoll_create1(int flags) {
    ++total_creates; if (epoll_error) { errno = epoll_error; return -1; } return __real_epoll_create1(flags);
}
int __real_eventfd(unsigned int,int);
int __wrap_eventfd(unsigned int value, int flags) {
    ++total_creates; if (event_error) { errno = event_error; return -1; } return __real_eventfd(value,flags);
}
int __real_epoll_ctl(int,int,int,epoll_event *);
int __wrap_epoll_ctl(int fd, int op, int target, epoll_event *event) {
    if (ctl_calls++ == ctl_error_at) { errno = EIO; return -1; } return __real_epoll_ctl(fd,op,target,event);
}
int __real_epoll_wait(int,epoll_event *,int,int);
int __wrap_epoll_wait(int fd, epoll_event *events, int count, int timeout) {
    if (wait_error) { errno = wait_error; return -1; } return __real_epoll_wait(fd,events,count,timeout);
}
int __real_accept4(int,sockaddr *,socklen_t *,int);
int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags) {
    const int call = accept_calls++;
    if (accept_after >= 0 && call >= accept_after && (accept_until < 0 || call < accept_until) && accept_error) {
        errno = accept_error; return -1;
    }
    const int accepted = __real_accept4(fd,address,size,flags);
    if (accepted >= 0) {
        observed_accepted = accepted;
        if (close_on_accept) { close_on_accept->close(); close_on_accept = nullptr; }
    }
    return accepted;
}
int __real_setsockopt(int,int,int,const void *,socklen_t);
int __wrap_setsockopt(int fd, int level, int option, const void *value, socklen_t size) {
    if (level == IPPROTO_TCP && option == TCP_NODELAY && configure_error) { errno = configure_error; return -1; }
    return __real_setsockopt(fd,level,option,value,size);
}
ssize_t __real_write(int,const void *,std::size_t);
ssize_t __wrap_write(int fd, const void *data, std::size_t size) {
    if (fd == watched_write && write_error) { errno = write_error; return -1; } return __real_write(fd,data,size);
}
int __real_close(int);
int __wrap_close(int fd) {
    const int result = __real_close(fd);
    if (fd == watched_close || fd == observed_accepted) {
        ++close_calls; closed_in = std::this_thread::get_id();
        if (close_error) { errno = close_error; return -1; }
    }
    return result;
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (lock_calls++ == lock_error_at) { lock_error_at = -1; return EAGAIN; } return __real_pthread_mutex_lock(mutex);
}
void *__real__Znwm(std::size_t);
void *__wrap__Znwm(std::size_t size) {
    if (allocation_calls++ == allocation_error_at) { allocation_error_at = -1; throw std::bad_alloc(); } return __real__Znwm(size);
}
}
int main() {
    const auto baseline = fd_count();
    {
        FdHandoffQueue queue(1); auto fd = notification(); const std::vector<MainReactorTarget> good{{&queue,fd.get()}};
        std::array<std::vector<MainReactorTarget>,5> invalid{{{},{{nullptr,fd.get()}},{{&queue,-1}},{{&queue,fd.get()},{&queue,fd.get()+1}},{{&queue,fd.get()},{nullptr,fd.get()}}}};
        for (const auto &targets : invalid) {
            total_creates = 0; auto result = MainReactor::create(targets);
            require(std::holds_alternative<std::error_code>(result) && std::get<std::error_code>(result).value() == EINVAL && total_creates == 0,
                "complete precheck before resource creation");
        }
        for (const auto budget : {std::size_t{0},std::size_t{1025}}) {
            MainReactorConfig config; config.accept_attempt_budget = budget; total_creates = 0;
            auto result = MainReactor::create(good,{},config);
            require(std::get<std::error_code>(result).value() == EINVAL && total_creates == 0, "invalid attempt budget");
        }
        FdHandoffQueue other(1);
        total_creates = 0; auto duplicate = MainReactor::create({{&queue,fd.get()},{&other,fd.get()}});
        require(std::get<std::error_code>(duplicate).value() == EINVAL && total_creates == 0, "distinct queues cannot share wakeup number");
        std::vector<MainReactorTarget> too_many(65, good.front());
        total_creates = 0; auto oversized = MainReactor::create(too_many);
        require(std::get<std::error_code>(oversized).value() == EINVAL && total_creates == 0, "target limit precheck");
        // 分配失败覆盖借用表复制与完整资源创建后的对象分配；目标入口/资源不能被回滚清理。
        for (const int index : {0,1}) {
            allocation_calls = 0; allocation_error_at = index; bool failed = false;
            try { (void)MainReactor::create(good); } catch (const std::bad_alloc &) { failed = true; }
            allocation_error_at = -1;
            require(failed && !queue.status().closed && !notify_eventfd(fd.get()), "allocation rollback retains targets");
        }
        for (int phase = 0; phase < 5; ++phase) {
            socket_error = phase == 0 ? EMFILE : 0; epoll_error = phase == 1 ? ENFILE : 0; event_error = phase == 2 ? ENOMEM : 0;
            ctl_calls = 0; ctl_error_at = phase == 3 ? 0 : phase == 4 ? 1 : -1;
            auto result = MainReactor::create(good);
            require(std::holds_alternative<std::error_code>(result) && std::get<std::error_code>(result).value() ==
                (phase == 0 ? EMFILE : phase == 1 ? ENFILE : phase == 2 ? ENOMEM : EIO), "startup exact error");
            socket_error = epoll_error = event_error = 0; ctl_error_at = -1;
            require(!queue.status().closed && !notify_eventfd(fd.get()), "startup failure leaves borrowed resources intact");
        }
        queue.close(); total_creates = 0; auto result = MainReactor::create(good);
        require(std::get<std::error_code>(result).value() == ECANCELED && total_creates == 0, "closed target cannot open listener");
    }
    {
        FdHandoffQueue queue(1); auto fd = notification(); auto main = create({{&queue,fd.get()}},false,3);
        auto peer = client(main->port()); accept_calls = 0; accept_after = 0; accept_until = 2; accept_error = EINTR;
        const auto step = main->run_once(100); accept_after = accept_until = -1; accept_error = 0;
        require(step.accept_attempts == 3 && step.handed_off == 1 && step.accept_budget_exhausted, "interrupted attempts then real success share budget");
        auto item = queue.try_pop(); require(item && !item->close(), "target owns after retry"); observed_accepted = -1;
    }
    {
        FdHandoffQueue queue(1); auto fd = notification(); auto main = create({{&queue,fd.get()}});
        wait_error = EINTR; auto step = main->run_once(0);
        require(step.stop == MainReactorStop::interrupted && step.error.value() == EINTR && main->port(), "wait EINTR preserves Main");
        wait_error = EIO; step = main->run_once(0); wait_error = 0;
        require(step.stop == MainReactorStop::failed && step.error.value() == EIO && main->port() == 0, "wait error terminates");
        require(main->run_once(0).error.value() == EIO && main->run_once(0).accept_attempts == 0, "terminal error stable zero I/O");
    }
    for (const int error : {EINTR,ECONNABORTED,EMFILE,ENFILE,ENOBUFS,ENOMEM}) {
        FdHandoffQueue queue(4); auto fd = notification(); auto main = create({{&queue,fd.get()}},false,3); auto peer = client(main->port());
        accept_calls = 0; accept_after = 0; accept_error = error;
        const auto step = main->run_once(100); accept_after = -1; accept_error = 0;
        const bool transient = error == EINTR || error == ECONNABORTED;
        require(step.accept_attempts == (transient ? 3U : 1U) && step.accepted == 0 &&
            (transient ? step.accept_budget_exhausted && step.stop == MainReactorStop::progress : step.stop == MainReactorStop::failed && step.error.value() == error),
            "accept finite transient retry or immediate resource failure");
    }
    {
        FdHandoffQueue queue(4); auto fd = notification(); auto main = create({{&queue,fd.get()}});
        auto first = client(main->port()), second = client(main->port());
        accept_calls = 0; accept_after = 1; accept_error = EMFILE;
        const auto step = main->run_once(100); accept_after = -1; accept_error = 0;
        require(step.stop == MainReactorStop::failed && step.error.value() == EMFILE && step.accepted == 1 && step.handed_off == 1 && queue.status().size == 1,
            "partial handoff retained before resource failure");
        auto item = queue.try_pop(); require(item && !item->close(), "target cleans partial committed fd");
    }
    for (const bool notification_failure : {false,true}) {
        FdHandoffQueue queue(1); auto fd = notification(); auto main = create({{&queue,fd.get()}},!notification_failure); auto peer = client(main->port());
        observed_accepted = -1; close_calls = 0; watched_write = fd.get(); write_error = notification_failure ? EIO : 0;
        configure_error = notification_failure ? 0 : ENOPROTOOPT;
        const auto step = main->run_once(100); write_error = 0; configure_error = 0;
        require(step.stop == MainReactorStop::failed && step.accepted == 1 && step.error.value() == (notification_failure ? EIO : ENOPROTOOPT), "configure or notify original error");
        require(step.handed_off == (notification_failure ? 1U : 0U) && step.rejected == (notification_failure ? 0U : 1U) &&
            close_calls == (notification_failure ? 0 : 1), "committed notification failure never closes target socket");
        if (notification_failure) {
            auto item = queue.try_pop(); require(item && item->get() == observed_accepted && !item->close() && close_calls == 1, "consumer owns accepted fd after error");
        } else require(closed_in == std::this_thread::get_id(), "Main closes uncommitted configure failure");
        observed_accepted = -1; watched_write = -1;
    }
    {
        FdHandoffQueue queue(1); auto fd = notification(); auto main = create({{&queue,fd.get()}}); auto peer = client(main->port());
        lock_calls = 0; lock_error_at = 1; close_calls = 0; observed_accepted = -1;
        const auto step = main->run_once(100); lock_error_at = -1;
        require(step.stop == MainReactorStop::failed && step.error.value() == EAGAIN && step.accepted == 1 && step.rejected == 1 &&
            step.handed_off == 0 && queue.status().size == 0 && close_calls == 1, "handoff mutex failure before commit closes Main source once");
        observed_accepted = -1;
    }
    {
        FdHandoffQueue queue(1); auto fd = notification(); auto main = create({{&queue,fd.get()}}); auto peer = client(main->port());
        close_on_accept = &queue; close_calls = 0; observed_accepted = -1; close_error = EINTR;
        const auto step = main->run_once(100); close_error = 0;
        require(step.stop == MainReactorStop::failed && step.error.value() == ECANCELED && step.cleanup_error.value() == EINTR &&
            step.accepted == 1 && step.rejected == 1 && close_calls == 1, "closed between readiness and commit rejects; cleanup distinct");
        require(main->run_once(0).cleanup_error.value() == EINTR && close_calls == 1, "close EINTR never retried");
        observed_accepted = -1;
    }
    {
        FdHandoffQueue q0(1), q1(1); auto n0 = notification(), n1 = notification();
        auto main = create({{&q0,n0.get()},{&q1,n1.get()}});
        lock_calls = 0; lock_error_at = 1;
        require(main->close().value() == EAGAIN, "target cleanup mutex failure diagnosed"); lock_error_at = -1;
        require(!q0.status().closed && q1.status().closed && main->port() == 0, "remaining cleanup proceeds; failed target needs controller");
        require(main->close().value() == EAGAIN && !q0.status().closed, "idempotent Main close does not pretend failed target closed");
        q0.close(); require(!notify_eventfd(n0.get()), "controller still owns target endpoint");
    }
    require(fd_count() == baseline, "all startup/run failure resources reclaimed");
    std::cout << "Main injection checks=" << checks << '\n';
}
