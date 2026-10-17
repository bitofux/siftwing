/*
 * PROJECT : SIFTWING
 * FILE    : eventfd_wakeup_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 系统调用注入验证有限重试、部分进展与提交后通知失败的所有权
 */
#include "siftwing/net/eventfd_wakeup.hpp"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
// 注入只在主线程、只拦目标fd；脚本0表示调用真实内核，正值errno，负值模拟短读写。
thread_local int watched = -1, create_error = 0, lock_error = 0;
thread_local std::array<int, 70> script{};
thread_local std::size_t length = 0, offset = 0;
thread_local bool reading = false, close_eintr = false;
thread_local unsigned int calls = 0, closes = 0;
unsigned int checks = 0;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "eventfd injection failed: " << label << '\n';
        std::exit(1);
    }
}
void program(bool read_mode, int code, std::size_t repeat = 1) {
    script.fill(0);
    length = repeat;
    offset = 0;
    calls = 0;
    reading = read_mode;
    for (std::size_t i = 0; i < repeat; ++i)
        script[i] = code;
}
ssize_t injected() {
    const int code = script[offset++];
    if (code > 0) {
        errno = code;
        return -1;
    }
    return static_cast<ssize_t>(-code - 1); // -1为0字节，-4为3字节。
}
UniqueFd resource() {
    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    require(static_cast<bool>(fd), "independent resource");
    return fd;
}
} // namespace
extern "C" {
int __real_eventfd(unsigned int, int);
int __wrap_eventfd(unsigned int value, int flags) {
    if (create_error) {
        errno = create_error;
        return -1;
    }
    return __real_eventfd(value, flags);
}
ssize_t __real_write(int, const void *, std::size_t);
ssize_t __wrap_write(int fd, const void *data, std::size_t size) {
    if (fd == watched && !reading) {
        ++calls;
        if (offset < length && script[offset] != 0)
            return injected();
        if (offset < length)
            ++offset;
    }
    return __real_write(fd, data, size);
}
ssize_t __real_read(int, void *, std::size_t);
ssize_t __wrap_read(int fd, void *data, std::size_t size) {
    if (fd == watched && reading) {
        ++calls;
        if (offset < length && script[offset] != 0)
            return injected();
        if (offset < length)
            ++offset;
    }
    return __real_read(fd, data, size);
}
int __real_close(int);
int __wrap_close(int fd) {
    if (fd == watched) {
        ++closes;
        const int result = __real_close(fd);
        // Linux close即使报告EINTR也已释放槽位；不能重试或重新接管数字。
        if (close_eintr) {
            errno = EINTR;
            return -1;
        }
        return result;
    }
    return __real_close(fd);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (lock_error) {
        const int error = lock_error;
        lock_error = 0;
        return error;
    }
    return __real_pthread_mutex_lock(mutex);
}
}
int main() {
    for (const int error : {EMFILE, ENFILE, ENOMEM, EINVAL, EINTR}) {
        create_error = error;
        auto result = create_eventfd_wakeup();
        require(std::holds_alternative<std::error_code>(result)
                    && std::get<std::error_code>(result)
                           == std::error_code(error, std::generic_category()),
            "create original errno");
    }
    create_error = 0;
    auto created = create_eventfd_wakeup();
    require(std::holds_alternative<UniqueFd>(created), "create real owner");
    auto wakeup = std::move(std::get<UniqueFd>(created));
    watched = wakeup.get();
    program(false, EINTR, 2);
    require(
        !notify_eventfd(watched) && calls == 3, "write retries interrupted calls then succeeds");
    program(true, EINTR, 2);
    auto drained = drain_eventfd(watched);
    require(
        drained.state == EventFdDrainState::drained && drained.successful_reads == 1 && calls == 4,
        "read retries EINTR then real aggregate and EAGAIN");
    program(false, EINTR, eventfd_attempt_limit);
    require(notify_eventfd(watched).value() == EINTR && calls == eventfd_attempt_limit,
        "write finite interrupt budget");
    program(true, EINTR, eventfd_attempt_limit);
    drained = drain_eventfd(watched);
    require(drained.state == EventFdDrainState::budget_exhausted && drained.successful_reads == 0
                && !drained.error && calls == eventfd_attempt_limit,
        "read finite interrupt budget");
    for (const int code : {EAGAIN, EBADF, EIO, -1, -4}) {
        program(false, code);
        const auto error = notify_eventfd(watched);
        require(error.value()
                        == (code == EAGAIN ? 0
                            : code < 0     ? EIO
                                           : code)
                    && calls == 1,
            "write error and short result");
        program(true, code);
        drained = drain_eventfd(watched);
        require(drained.state
                        == (code == EAGAIN ? EventFdDrainState::drained : EventFdDrainState::failed)
                    && drained.error.value()
                           == (code == EAGAIN ? 0
                               : code < 0     ? EIO
                                              : code)
                    && calls == 1,
            "read error EOF and short result");
    }
    program(false, 0, 0);
    require(!notify_eventfd(watched), "partial progress seed");
    program(true, 0, 2);
    script[1] = EIO;
    drained = drain_eventfd(watched);
    require(drained.state == EventFdDrainState::failed && drained.successful_reads == 1
                && drained.error.value() == EIO,
        "partial progress retained on error");
    {
        FdHandoffQueue queue(1);
        auto source = resource();
        const int original = source.get();
        program(false, EIO);
        const auto result = try_handoff_and_notify(queue, source, watched);
        require(result.handoff == FdHandoffResult::accepted
                    && result.notification_error.value() == EIO && !source && calls == 1,
            "notification error never revokes accepted ownership");
        auto rejected = resource();
        const int kept = rejected.get();
        calls = 0;
        require(try_handoff_and_notify(queue, rejected, watched).handoff == FdHandoffResult::full
                    && rejected.get() == kept && calls == 0,
            "full never notifies and retains source");
        UniqueFd empty;
        require(try_handoff_and_notify(queue, empty, watched).handoff == FdHandoffResult::invalid_fd
                    && calls == 0,
            "invalid never notifies");
        queue.close();
        const auto closed = try_handoff_and_notify(queue, rejected, watched);
        require(closed.handoff == FdHandoffResult::closed && !closed.notification_error
                    && rejected.get() == kept && calls == 0,
            "closed never notifies");
        auto item = queue.try_pop();
        require(item && item->get() == original && !item->close(),
            "queue retains exactly one accepted owner after notify error");
    }
    {
        FdHandoffQueue queue(1);
        auto source = resource();
        const int original = source.get();
        calls = 0;
        lock_error = EAGAIN;
        bool failed = false;
        try {
            (void)try_handoff_and_notify(queue, source, watched);
        } catch (const std::system_error &error) {
            failed = error.code().value() == EAGAIN;
        }
        require(failed && source.get() == original && calls == 0 && queue.status().size == 0,
            "mutex exception before commit never notifies");
    }
    close_eintr = true;
    closes = 0;
    require(wakeup.close().value() == EINTR && !wakeup && closes == 1,
        "owner cleared before close error");
    require(!wakeup.close() && closes == 1, "never retry close");
    watched = -1;
    std::cout << "eventfd injection checks=" << checks << '\n';
}
