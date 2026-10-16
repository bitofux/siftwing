/*
 * PROJECT : SIFTWING
 * FILE    : signalfd_server_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 仅测试链接的正式入口信号启动窗口与失败门禁
 */
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace {
std::atomic<int> signal_fd{-1};
bool mode(const char *name) {
    const char *value = std::getenv("SIFTWING_TEST_SIGNAL_MODE");
    return value && std::strcmp(value, name) == 0;
}
}
extern "C" {
int __real_pthread_sigmask(int, const sigset_t *, sigset_t *);
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
    const bool managed = how == SIG_BLOCK && set && ::sigismember(set, SIGINT) == 1
                         && ::sigismember(set, SIGTERM) == 1;
    if (managed && mode("block_failure")) {
        errno = EIO;
        return EAGAIN;
    }
    const int error = __real_pthread_sigmask(how, set, old);
    // 在真实block之后、任何日志/快照/线程启动之前产生pending信号。
    if (managed && error == 0
        && (mode("startup_term") || mode("create_failure") || mode("add_failure")))
        (void)::kill(::getpid(), SIGTERM);
    return error;
}
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(
    pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg) {
    sigset_t set;
    if (__real_pthread_sigmask(SIG_BLOCK, nullptr, &set) != 0 || ::sigismember(&set, SIGINT) != 1
        || ::sigismember(&set, SIGTERM) != 1)
        return EINVAL;
    return __real_pthread_create(thread, attr, entry, arg);
}
int __real_signalfd(int, const sigset_t *, int);
int __wrap_signalfd(int fd, const sigset_t *set, int flags) {
    if (mode("create_failure")) {
        errno = EMFILE;
        return -1;
    }
    const int made = __real_signalfd(fd, set, flags);
    signal_fd = made;
    return made;
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event) {
    if (op == EPOLL_CTL_ADD && fd == signal_fd.load() && mode("add_failure")) {
        errno = ENOSPC;
        return -1;
    }
    return __real_epoll_ctl(epoll, op, fd, event);
}
int __real_close(int);
int __wrap_close(int fd) {
    if (fd == signal_fd.load()) {
        signal_fd = -1;
        if (mode("close_pending")) {
            (void)::kill(::getpid(), SIGTERM);
            (void)::kill(::getpid(), SIGINT);
        }
    }
    return __real_close(fd);
}
}
