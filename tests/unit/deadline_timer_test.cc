/*
 * PROJECT : SIFTWING
 * FILE    : deadline_timer_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 真实timerfd/epoll闭环与仓内静态链接边界的系统调用故障
 */
#include "siftwing/net/deadline_timer.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace siftwing::net;
unsigned int checks = 0, create_calls = 0, set_calls = 0, read_calls = 0;
int create_error = 0, set_error = 0, read_mode = 0, clock_mode = 0;
bool created_correctly = true, set_correctly = true;
int watched = -1;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        std::cerr << "deadline timer failed: " << label << '\n';
        std::exit(1);
    }
}
DeadlineTime now() {
    auto value = monotonic_now();
    check(std::holds_alternative<DeadlineTime>(value), "read monotonic clock");
    return std::get<DeadlineTime>(value);
}
UniqueFd timer() {
    auto result = create_deadline_timer();
    check(std::holds_alternative<UniqueFd>(result), "create timer");
    return std::move(std::get<UniqueFd>(result));
}
void rearm(int fd, const DeadlineQueue &queue) {
    const auto next = queue.next();
    check(!arm_deadline_timer(fd, next ? std::optional<DeadlineTime>(next->when) : std::nullopt),
        "arm minimum or empty");
}
} // namespace
extern "C" {
int __real_timerfd_create(int, int);
int __wrap_timerfd_create(int clock, int flags) {
    ++create_calls;
    created_correctly =
        created_correctly && clock == CLOCK_MONOTONIC && flags == (TFD_CLOEXEC | TFD_NONBLOCK);
    if (create_error) {
        errno = create_error;
        return -1;
    }
    return __real_timerfd_create(clock, flags);
}
int __real_timerfd_settime(int, int, const itimerspec *, itimerspec *);
int __wrap_timerfd_settime(int fd, int flags, const itimerspec *value, itimerspec *old) {
    ++set_calls;
    set_correctly = set_correctly && flags == TFD_TIMER_ABSTIME && old == nullptr
                    && value->it_interval.tv_sec == 0 && value->it_interval.tv_nsec == 0;
    if (set_error) {
        errno = set_error;
        return -1;
    }
    return __real_timerfd_settime(fd, flags, value, old);
}
int __real_clock_gettime(clockid_t, timespec *);
int __wrap_clock_gettime(clockid_t clock, timespec *value) {
    if (clock != CLOCK_MONOTONIC || clock_mode == 0)
        return __real_clock_gettime(clock, value);
    if (clock_mode == 1) {
        errno = EIO;
        return -1;
    }
    const auto maximum = std::numeric_limits<DeadlineTime>::max();
    value->tv_sec = static_cast<time_t>(maximum / 1000000000);
    value->tv_nsec = static_cast<long>(maximum % 1000000000);
    if (clock_mode == 2)
        ++value->tv_nsec;
    if (clock_mode == 3) {
        value->tv_sec = 0;
        value->tv_nsec = 1000000000;
    }
    if (clock_mode == 4)
        value->tv_sec = -1;
    return 0;
}
ssize_t __real_read(int, void *, std::size_t);
ssize_t __wrap_read(int fd, void *bytes, std::size_t size) {
    if (fd == watched) {
        ++read_calls;
        if (read_mode == 1) {
            read_mode = 0;
            errno = EINTR;
            return -1;
        }
        if (read_mode == 2) {
            errno = EINTR;
            return -1;
        }
        if (read_mode == 3)
            return 7;
        if (read_mode == 4)
            return 0;
        if (read_mode == 5) {
            errno = EIO;
            return -1;
        }
    }
    return __real_read(fd, bytes, size);
}
}
int main() {
    for (const int error : {EINTR, EMFILE}) {
        create_error = error;
        const auto before = create_calls;
        const auto result = create_deadline_timer();
        check(std::get<std::error_code>(result).value() == error && create_calls == before + 1,
            "create preserves errno and never retries");
    }
    create_error = 0;
    clock_mode = 1;
    check(std::get<std::error_code>(monotonic_now()).value() == EIO, "clock error");
    for (const int mode : {2, 3, 4}) {
        clock_mode = mode;
        check(std::get<std::error_code>(monotonic_now()).value() == EOVERFLOW,
            "clock conversion overflow or invalid");
    }
    clock_mode = 5;
    check(std::get<DeadlineTime>(monotonic_now()) == std::numeric_limits<DeadlineTime>::max(),
        "clock maximum exact");
    clock_mode = 0;
    const auto first_now = now();
    const auto second_now = now();
    check(first_now <= second_now, "same monotonic clock does not move backward");
    {
        auto fd = timer();
        watched = fd.get();
        check(
            (::fcntl(fd.get(), F_GETFL) & O_NONBLOCK) && (::fcntl(fd.get(), F_GETFD) & FD_CLOEXEC),
            "fd flags");
        auto poll_result = create_epoll();
        check(std::holds_alternative<UniqueFd>(poll_result), "create epoll");
        auto poll = std::move(std::get<UniqueFd>(poll_result));
        check(!epoll_add(poll.get(), fd.get(), EPOLLIN, 17), "register timer");
        epoll_event event{};
        check(::epoll_wait(poll.get(), &event, 1, 0) == 0, "initial disarmed");
        check(drain_deadline_timer(fd.get()).state == TimerDrainState::drained, "empty EAGAIN");
        check(arm_deadline_timer(-1, 0).value() == EBADF
                  && arm_deadline_timer(fd.get(), 0).value() == EINVAL
                  && arm_deadline_timer(fd.get(), -1).value() == EINVAL,
            "invalid arm precedence");
        for (const int error : {EINTR, EINVAL}) {
            set_error = error;
            const auto before = set_calls;
            check(arm_deadline_timer(fd.get(), 1).value() == error && set_calls == before + 1,
                "settime failure no silent retry");
        }
        set_error = 0;
        DeadlineQueue queue(3);
        const auto origin = now();
        check(origin <= std::numeric_limits<DeadlineTime>::max() - 120000000000LL,
            "future test arithmetic safe");
        check(
            queue.upsert({9, origin + 120000000000LL}) == DeadlineUpdate::stored, "later deadline");
        rearm(fd.get(), queue);
        check(queue.upsert({4, origin + 60000000000LL}) == DeadlineUpdate::stored,
            "earlier deadline");
        rearm(fd.get(), queue);
        itimerspec remaining{};
        check(::timerfd_gettime(fd.get(), &remaining) == 0 && remaining.it_interval.tv_sec == 0
                  && remaining.it_interval.tv_nsec == 0 && remaining.it_value.tv_sec > 0
                  && remaining.it_value.tv_sec <= 60,
            "kernel armed to nearest, one shot");
        check(queue.upsert({4, origin + 120000000000LL}) == DeadlineUpdate::stored,
            "postpone earliest");
        rearm(fd.get(), queue);
        check(::timerfd_gettime(fd.get(), &remaining) == 0 && remaining.it_value.tv_sec > 60,
            "postpone resets kernel minimum");
        check(queue.erase(4) && queue.erase(9), "cancel all");
        rearm(fd.get(), queue);
        check(::timerfd_gettime(fd.get(), &remaining) == 0 && remaining.it_value.tv_sec == 0
                  && remaining.it_value.tv_nsec == 0,
            "empty explicitly disarmed");
        for (const std::uint64_t key : {9U, 4U, 6U})
            check(queue.upsert({key, 1}) == DeadlineUpdate::stored, "three already due");
        rearm(fd.get(), queue);
        check(::epoll_wait(poll.get(), &event, 1, 2000) == 1 && event.data.u64 == 17
                  && (event.events & EPOLLIN),
            "real past absolute deadline wakes epoll");
        const auto drained = drain_deadline_timer(fd.get(), 1);
        check(drained.state == TimerDrainState::budget_exhausted && drained.successful_reads == 1,
            "one kernel read is not three deadlines");
        check(drain_deadline_timer(fd.get()).state == TimerDrainState::drained,
            "observe EAGAIN after read budget");
        const auto observed = now();
        for (const std::uint64_t key : {4U, 6U, 9U}) {
            const auto due = queue.pop_due(observed);
            check(due && due->key == key, "container supplies three due entries");
        }
        check(!queue.pop_due(observed), "each due consumed once");
        rearm(fd.get(), queue);
        check(::epoll_wait(poll.get(), &event, 1, 0) == 0, "no LT busy loop after read and disarm");
        check(!arm_deadline_timer(fd.get(), 1), "seed stale batch");
        check(::epoll_wait(poll.get(), &event, 1, 2000) == 1, "obtain readable batch");
        check(!arm_deadline_timer(fd.get(), std::nullopt), "cancel after batch");
        check(drain_deadline_timer(fd.get()).state == TimerDrainState::drained,
            "stale readiness after cancellation normal");
        check(drain_deadline_timer(-1, 0).error.value() == EBADF
                  && drain_deadline_timer(fd.get(), 0).error.value() == EINVAL,
            "drain preconditions");
        read_mode = 1;
        check(drain_deadline_timer(fd.get(), 2).state == TimerDrainState::drained,
            "EINTR retries to EAGAIN");
        read_mode = 2;
        const auto reads_before = read_calls;
        const auto budget = drain_deadline_timer(fd.get(), 3);
        check(budget.state == TimerDrainState::budget_exhausted && budget.successful_reads == 0
                  && read_calls == reads_before + 3,
            "persistent EINTR finite budget");
        for (const int mode : {3, 4, 5}) {
            read_mode = mode;
            check(drain_deadline_timer(fd.get()).error.value() == EIO,
                "short EOF and error cannot masquerade as drain");
        }
        read_mode = 0;
        check(!arm_deadline_timer(fd.get(), std::numeric_limits<DeadlineTime>::max()),
            "largest supported absolute nanoseconds");
        check(!arm_deadline_timer(fd.get(), std::nullopt) && !epoll_remove(poll.get(), fd.get()),
            "disarm and unregister");
        watched = -1;
    }
    const auto child = ::fork();
    check(child >= 0, "fork fd zero");
    if (child == 0) {
        (void)::close(STDIN_FILENO);
        auto result = create_deadline_timer();
        if (!std::holds_alternative<UniqueFd>(result))
            ::_exit(2);
        auto &fd = std::get<UniqueFd>(result);
        if (fd.get() != 0 || arm_deadline_timer(fd.get(), 1) || fd.close())
            ::_exit(3);
        ::_exit(0);
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fd zero valid owner");
    check(created_correctly && set_correctly, "all create and set flags match contract");
    std::cout << "deadline timer checks=" << checks << '\n';
}
