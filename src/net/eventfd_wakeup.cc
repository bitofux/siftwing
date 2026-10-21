/*
 * PROJECT : SIFTWING
 * FILE    : eventfd_wakeup.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 通知计数与真实队列分离，系统调用采用有限尝试预算
 */
#include "siftwing/net/eventfd_wakeup.hpp"
#include <cerrno>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

namespace siftwing::net {
namespace {
std::error_code diagnostic(int value) noexcept {
    return {value, std::generic_category()};
}
} // namespace
EventFdResult create_eventfd_wakeup() noexcept {
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
        return diagnostic(errno);
    return UniqueFd(fd);
}
std::error_code notify_eventfd(int stable_fd) noexcept {
    if (stable_fd < 0)
        return diagnostic(EBADF);
    const std::uint64_t one = 1;
    for (std::size_t attempt = 0; attempt < eventfd_attempt_limit; ++attempt) {
        const auto bytes = ::write(stable_fd, &one, sizeof(one));
        if (bytes == static_cast<ssize_t>(sizeof(one)))
            return {};
        if (bytes >= 0)
            return diagnostic(EIO);
        const int error = errno;
        if (error == EINTR)
            continue;
        // eventfd饱和时已可读；不等待容量，不把已提交条目收回。
        if (error == EAGAIN)
            return {};
        return diagnostic(error);
    }
    return diagnostic(EINTR);
}
EventFdDrainResult drain_eventfd(int stable_fd, std::size_t max_attempts) noexcept {
    if (stable_fd < 0)
        return {EventFdDrainState::failed, 0, diagnostic(EBADF)};
    if (max_attempts == 0)
        return {EventFdDrainState::failed, 0, diagnostic(EINVAL)};
    std::size_t reads = 0;
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        std::uint64_t ignored = 0;
        const auto bytes = ::read(stable_fd, &ignored, sizeof(ignored));
        if (bytes == static_cast<ssize_t>(sizeof(ignored))) {
            ++reads;
            continue;
        }
        if (bytes >= 0)
            return {EventFdDrainState::failed, reads, diagnostic(EIO)};
        const int error = errno;
        if (error == EINTR)
            continue;
        if (error == EAGAIN)
            return {EventFdDrainState::drained, reads, {}};
        return {EventFdDrainState::failed, reads, diagnostic(error)};
    }
    // 持续发布或持续信号不能让本调用无限占用消费者线程。
    return {EventFdDrainState::budget_exhausted, reads, {}};
}

} // namespace siftwing::net
