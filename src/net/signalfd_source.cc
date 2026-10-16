/*
 * PROJECT : SIFTWING
 * FILE    : signalfd_source.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 受管终止信号转为有限读取的Linux控制事件
 */
#include "siftwing/net/signalfd_source.hpp"
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace siftwing::net {
namespace {
std::error_code code(int error) noexcept {
    return {error, std::generic_category()};
}
sigset_t termination_set() noexcept {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGINT);
    ::sigaddset(&set, SIGTERM);
    return set;
}
} // namespace
std::error_code block_termination_signals() noexcept {
    const auto set = termination_set();
    // pthread接口直接返回错误码，errno可能是此前无关系统调用留下的值。
    return code(::pthread_sigmask(SIG_BLOCK, &set, nullptr));
}
SignalFdResult create_termination_signalfd() noexcept {
    sigset_t current;
    if (const int error = ::pthread_sigmask(SIG_BLOCK, nullptr, &current))
        return code(error);
    if (::sigismember(&current, SIGINT) != 1 || ::sigismember(&current, SIGTERM) != 1)
        return code(EINVAL);
    const auto set = termination_set();
    const int fd = ::signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0)
        return code(errno);
    return UniqueFd(fd);
}
SignalFdDrainResult drain_termination_signalfd(int stable_fd, std::size_t max_attempts) noexcept {
    SignalFdDrainResult result{SignalFdDrainState::failed, 0, 0, {}};
    if (stable_fd < 0) {
        result.error = code(EBADF);
        return result;
    }
    if (max_attempts == 0) {
        result.error = code(EINVAL);
        return result;
    }
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        signalfd_siginfo record{};
        const auto bytes = ::read(stable_fd, &record, sizeof(record));
        if (bytes == static_cast<ssize_t>(sizeof(record))) {
            if (record.ssi_signo != SIGINT && record.ssi_signo != SIGTERM) {
                result.error = code(EIO);
                return result;
            }
            if (result.signal_number == 0)
                result.signal_number = static_cast<int>(record.ssi_signo);
            ++result.records;
            continue;
        }
        if (bytes >= 0) {
            result.error = code(EIO);
            return result;
        }
        const int error = errno;
        if (error == EINTR)
            continue;
        if (error == EAGAIN) {
            result.state = SignalFdDrainState::drained;
            return result;
        }
        result.error = code(error);
        return result;
    }
    result.state = SignalFdDrainState::budget_exhausted;
    return result;
}
} // namespace siftwing::net
