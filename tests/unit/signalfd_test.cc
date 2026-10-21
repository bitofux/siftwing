/*
 * PROJECT : SIFTWING
 * FILE    : signalfd_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 真实信号与可控系统错误证明有限读取、同批优先及资源回滚
 */
#include "siftwing/net/signalfd_source.hpp"
#include "siftwing/net/eventfd_wakeup.hpp"
#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace siftwing::net;
unsigned checks = 0;
thread_local int mask_error = 0, create_error = 0, read_error = 0, short_read = -1;
thread_local int watched = -1, fake_signal = 0, ctl_failure = -1, adds = 0;
thread_local int wait_error = 0;
thread_local unsigned reads = 0, closes = 0, deletes = 0, waits = 0;
thread_local bool corrupt_batch = false, deleted = false, close_order = true;
void check(bool ok, const char *why) {
    ++checks;
    if (!ok) {
        std::cerr << "signalfd: " << why << '\n';
        std::exit(1);
    }
}
std::size_t fd_count() {
    std::size_t n = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++n;
    }
    return n;
}
UniqueFd signal_fd() {
    auto r = create_termination_signalfd();
    check(std::holds_alternative<UniqueFd>(r), "real signal source");
    return std::move(std::get<UniqueFd>(r));
}

void clear_pending() {
    auto fd = signal_fd();
    check(drain_termination_signalfd(fd.get()).state == SignalFdDrainState::drained,
        "clear pending in isolated test process");
}
} // namespace
extern "C" {
int __real_pthread_sigmask(int, const sigset_t *, sigset_t *);
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
    if (mask_error) {
        errno = EIO;
        return mask_error;
    }
    return __real_pthread_sigmask(how, set, old);
}
int __real_signalfd(int, const sigset_t *, int);
int __wrap_signalfd(int fd, const sigset_t *set, int flags) {
    if (create_error) {
        errno = create_error;
        return -1;
    }
    const int made = __real_signalfd(fd, set, flags);
    if (made >= 0) {
        watched = made;
        deleted = false;
    }
    return made;
}
ssize_t __real_read(int, void *, std::size_t);
ssize_t __wrap_read(int fd, void *data, std::size_t size) {
    if (fd == watched) {
        ++reads;
        if (read_error) {
            errno = read_error;
            return -1;
        }
        if (short_read >= 0)
            return short_read;
        if (fake_signal) {
            auto *record = static_cast<signalfd_siginfo *>(data);
            *record = {};
            record->ssi_signo = static_cast<std::uint32_t>(fake_signal);
            return static_cast<ssize_t>(sizeof(*record));
        }
    }
    return __real_read(fd, data, size);
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event) {
    if (op == EPOLL_CTL_ADD && adds++ == ctl_failure) {
        errno = ENOSPC;
        return -1;
    }
    if (op == EPOLL_CTL_DEL && fd == watched) {
        ++deletes;
        deleted = true;
    }
    return __real_epoll_ctl(epoll, op, fd, event);
}
int __real_close(int);
int __wrap_close(int fd) {
    if (fd == watched) {
        ++closes;
        close_order = close_order && deleted;
    }
    return __real_close(fd);
}
int __real_epoll_wait(int, epoll_event *, int, int);
int __wrap_epoll_wait(int epoll, epoll_event *events, int count, int timeout) {
    ++waits;
    if (wait_error) {
        errno = wait_error;
        return -1;
    }
    const int n = __real_epoll_wait(epoll, events, count, timeout);
    // 将错误放在信号记录之后，证明整批错误优先于已读正常请求。
    if (corrupt_batch && n >= 1 && n < count) {
        events[n] = {};
        events[n].events = EPOLLERR;
        events[n].data.u64 = 1;
        return n + 1;
    }
    return n;
}
}
int main() {
    sigset_t original, unblocked;
    check(::pthread_sigmask(SIG_BLOCK, nullptr, &original) == 0, "save test mask");
    unblocked = original;
    ::sigdelset(&unblocked, SIGINT);
    ::sigdelset(&unblocked, SIGTERM);
    ::sigaddset(&unblocked, SIGUSR1);
    check(::pthread_sigmask(SIG_SETMASK, &unblocked, nullptr) == 0,
        "test starts with unblocked termination signals");
    struct sigaction disposition {};
    check(::sigaction(SIGTERM, nullptr, &disposition) == 0, "original disposition");
    const auto baseline = fd_count();
    check(std::get<std::error_code>(create_termination_signalfd()).value() == EINVAL,
        "reject missing mask");
    mask_error = EAGAIN;
    check(block_termination_signals().value() == EAGAIN, "pthread direct error beats errno");
    check(std::get<std::error_code>(create_termination_signalfd()).value() == EAGAIN,
        "mask query direct error");
    mask_error = 0;
    check(!block_termination_signals() && !block_termination_signals(), "idempotent block");
    struct sigaction after {};
    ::sigaction(SIGTERM, nullptr, &after);
    check(after.sa_handler == disposition.sa_handler && after.sa_flags == disposition.sa_flags,
        "disposition unchanged");
    bool inherited = false;
    std::thread thread([&] {
        sigset_t set;
        ::pthread_sigmask(SIG_BLOCK, nullptr, &set);
        inherited = ::sigismember(&set, SIGTERM) == 1 && ::sigismember(&set, SIGINT) == 1
                    && ::sigismember(&set, SIGUSR1) == 1;
    });
    thread.join();
    check(inherited, "thread inherits termination and unrelated mask");
    for (const int error : {EMFILE, ENFILE, ENOMEM, EINVAL, EINTR}) {
        create_error = error;
        check(std::get<std::error_code>(create_termination_signalfd()).value() == error,
            "create original errno injection");
    }
    create_error = 0;
    {
        auto fd = signal_fd();
        check(
            (::fcntl(fd.get(), F_GETFL) & O_NONBLOCK) && (::fcntl(fd.get(), F_GETFD) & FD_CLOEXEC),
            "atomic flags");
        check(drain_termination_signalfd(fd.get()).state == SignalFdDrainState::drained,
            "initial EAGAIN");
        check(drain_termination_signalfd(-1).error.value() == EBADF
                  && drain_termination_signalfd(fd.get(), 0).error.value() == EINVAL,
            "invalid read arguments");
        for (const int signo : {SIGINT, SIGTERM}) {
            check(::kill(::getpid(), signo) == 0, "send actual process signal");
            const auto r = drain_termination_signalfd(fd.get());
            check(r.state == SignalFdDrainState::drained && r.signal_number == signo
                      && r.records == 1 && !r.error,
                "real complete record then EAGAIN");
        }
        for (const int error : {EINTR, EIO, EBADF}) {
            read_error = error;
            reads = 0;
            const auto r = drain_termination_signalfd(fd.get());
            check(error == EINTR
                      ? r.state == SignalFdDrainState::budget_exhausted && reads == 64 && !r.error
                      : r.state == SignalFdDrainState::failed && reads == 1
                            && r.error.value() == error,
                "bounded read error");
        }
        read_error = 0;
        for (const int size : {0, 1, 127}) {
            short_read = size;
            check(drain_termination_signalfd(fd.get()).error.value() == EIO,
                "EOF or short record rejected");
        }
        short_read = -1;
        fake_signal = SIGUSR1;
        check(
            drain_termination_signalfd(fd.get()).error.value() == EIO, "unmanaged record rejected");
        fake_signal = SIGTERM;
        reads = 0;
        const auto r = drain_termination_signalfd(fd.get(), 3);
        check(r.state == SignalFdDrainState::budget_exhausted && r.records == 3
                  && r.signal_number == SIGTERM && reads == 3,
            "continuous records finite budget");
        fake_signal = 0;
        check(::kill(::getpid(), SIGTERM) == 0, "seed partial progress");
        const auto partial = drain_termination_signalfd(fd.get(), 1);
        read_error = EIO;
        check(partial.signal_number == SIGTERM
                  && partial.state == SignalFdDrainState::budget_exhausted
                  && drain_termination_signalfd(fd.get()).error.value() == EIO,
            "budget retains first record, next call can fail");
        read_error = 0;
    }
    watched = -1;
    // fd0在本单线程测试中临时借槽，恢复stdin后结束原语验证；生产组合另测。
    const int saved = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    check(saved >= 0 && ::close(STDIN_FILENO) == 0, "save stdin");
    {
        auto fd = signal_fd();
        check(fd.get() == 0, "fd zero owns valid signalfd");
    }
    watched = -1;
    check(::dup2(saved, STDIN_FILENO) == 0 && ::close(saved) == 0, "restore stdin");
    clear_pending();
    check(fd_count() == baseline, "all descriptors reclaimed");
    check(::pthread_sigmask(SIG_SETMASK, &original, nullptr) == 0,
        "restore test-only mask after draining pending");
    std::cout << checks << " signal source checks passed\n";
}
