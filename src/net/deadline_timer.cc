/*
 * PROJECT : SIFTWING
 * FILE    : deadline_timer.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 受检单调时钟换算、绝对单次设时与非阻塞读取
 */
#include "siftwing/net/deadline_timer.hpp"
#include <cerrno>
#include <limits>
#include <sys/timerfd.h>
#include <unistd.h>

namespace siftwing::net {
namespace {
constexpr DeadlineTime billion = 1000000000;
std::error_code code(int value) noexcept { return {value, std::generic_category()}; }
} // namespace
std::variant<DeadlineTime, std::error_code> monotonic_now() noexcept {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) < 0)
        return code(errno);
    const auto maximum = std::numeric_limits<DeadlineTime>::max();
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= billion
        || value.tv_sec > maximum / billion
        || (value.tv_sec == maximum / billion && value.tv_nsec > maximum % billion))
        return code(EOVERFLOW);
    return static_cast<DeadlineTime>(value.tv_sec) * billion + value.tv_nsec;
}
std::variant<UniqueFd, std::error_code> create_deadline_timer() noexcept {
    const int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return code(errno);
    return UniqueFd(fd);
}
std::error_code arm_deadline_timer(int stable_fd, std::optional<DeadlineTime> when) noexcept {
    if (stable_fd < 0)
        return code(EBADF);
    if (when && *when <= 0)
        return code(EINVAL);
    itimerspec setting{};
    if (when) {
        // Ubuntu/Linux的time_t足以容纳int64纳秒的整秒部分；不依赖steady_clock的未指定纪元。
        static_assert(std::numeric_limits<time_t>::max()
                      >= std::numeric_limits<DeadlineTime>::max() / billion);
        setting.it_value.tv_sec = static_cast<time_t>(*when / billion);
        setting.it_value.tv_nsec = static_cast<long>(*when % billion);
    }
    if (::timerfd_settime(stable_fd, TFD_TIMER_ABSTIME, &setting, nullptr) < 0)
        return code(errno);
    return {};
}
TimerDrainResult drain_deadline_timer(int stable_fd, std::size_t max_attempts) noexcept {
    if (stable_fd < 0)
        return {TimerDrainState::failed, 0, code(EBADF)};
    if (max_attempts == 0)
        return {TimerDrainState::failed, 0, code(EINVAL)};
    std::size_t reads = 0;
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        std::uint64_t ignored = 0;
        const auto count = ::read(stable_fd, &ignored, sizeof(ignored));
        if (count == static_cast<ssize_t>(sizeof(ignored))) {
            ++reads;
            continue;
        }
        if (count >= 0)
            return {TimerDrainState::failed, reads, code(EIO)};
        const int error = errno;
        if (error == EINTR)
            continue;
        if (error == EAGAIN)
            return {TimerDrainState::drained, reads, {}};
        return {TimerDrainState::failed, reads, code(error)};
    }
    return {TimerDrainState::budget_exhausted, reads, {}};
}
} // namespace siftwing::net
