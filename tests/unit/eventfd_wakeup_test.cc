/*
 * PROJECT : SIFTWING
 * FILE    : eventfd_wakeup_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 真实eventfd聚合、饱和、有限drain与epoll可读性合同
 */
#include "siftwing/net/eventfd_wakeup.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "eventfd failed: " << label << '\n';
        std::exit(1);
    }
}
UniqueFd wakeup() {
    auto result = create_eventfd_wakeup();
    require(std::holds_alternative<UniqueFd>(result), "create real eventfd");
    return std::move(std::get<UniqueFd>(result));
}
} // namespace
int main() {
    {
        auto fd = wakeup();
        require((::fcntl(fd.get(), F_GETFL) & O_NONBLOCK) != 0, "nonblocking");
        require((::fcntl(fd.get(), F_GETFD) & FD_CLOEXEC) != 0, "close on exec");
        const auto empty = drain_eventfd(fd.get());
        require(empty.state == EventFdDrainState::drained && empty.successful_reads == 0
                    && !empty.error,
            "empty EAGAIN is observation");
        for (unsigned int i = 0; i < 5; ++i)
            require(!notify_eventfd(fd.get()), "five notifications");
        std::uint64_t counter = 0;
        require(::read(fd.get(), &counter, sizeof(counter)) == static_cast<ssize_t>(sizeof(counter))
                    && counter == 5,
            "ordinary eventfd aggregates, not semaphore");
        require(!notify_eventfd(fd.get()), "budget seed");
        const auto limited = drain_eventfd(fd.get(), 1);
        require(limited.state == EventFdDrainState::budget_exhausted
                    && limited.successful_reads == 1 && !limited.error,
            "last successful read does not prove drained");
        require(
            drain_eventfd(fd.get()).state == EventFdDrainState::drained, "next observation clears");
        require(drain_eventfd(fd.get(), 0).error.value() == EINVAL, "zero budget");
        require(drain_eventfd(-1, 0).error.value() == EBADF && notify_eventfd(-1).value() == EBADF,
            "negative fd");

        auto epoll_result = create_epoll();
        require(std::holds_alternative<UniqueFd>(epoll_result), "real epoll");
        auto epoll = std::move(std::get<UniqueFd>(epoll_result));
        constexpr std::uint64_t token = 0x123456789abcdef0ULL;
        require(!epoll_add(epoll.get(), fd.get(), EPOLLIN, token), "register wakeup");
        epoll_event event{};
        require(::epoll_wait(epoll.get(), &event, 1, 0) == 0, "no initial readiness");
        require(!notify_eventfd(fd.get()), "real wakeup");
        require(::epoll_wait(epoll.get(), &event, 1, 0) == 1 && event.data.u64 == token
                    && (event.events & EPOLLIN),
            "notification causes immediate real epoll readiness");
        require(drain_eventfd(fd.get()).state == EventFdDrainState::drained, "clear readiness");
        require(::epoll_wait(epoll.get(), &event, 1, 0) == 0, "drained is no longer readable");

        // 独立参考：真实内核计数饱和仍可读；真实队列只有3项，不能按巨大计数pop。
        const auto saturated = std::numeric_limits<std::uint64_t>::max() - 1;
        require(::write(fd.get(), &saturated, sizeof(saturated))
                    == static_cast<ssize_t>(sizeof(saturated)),
            "saturate");
        FdHandoffQueue queue(3);
        for (int i = 0; i < 3; ++i) {
            UniqueFd source(::open("/dev/null", O_RDONLY | O_CLOEXEC));
            require(static_cast<bool>(source), "resource");
            const auto result = try_handoff_and_notify(queue, source, fd.get());
            require(result.handoff == FdHandoffResult::accepted && !source
                        && !result.notification_error,
                "real EAGAIN accepted as coalesced notification");
        }
        require(::epoll_wait(epoll.get(), &event, 1, 0) == 1, "saturated still readable");
        const auto drained = drain_eventfd(fd.get());
        require(drained.state == EventFdDrainState::drained && drained.successful_reads == 1,
            "counter ignored");
        unsigned int popped = 0;
        while (auto item = queue.try_pop()) {
            ++popped;
            require(!item->close(), "close queue owner");
        }
        require(popped == 3 && queue.status().size == 0, "queue determines work count");
        require(!epoll_remove(epoll.get(), fd.get()), "unregister before close");
    }
    // 隔离子进程取得fd 0，不改变测试主进程标准输入；_exit不依赖缓冲输出。
    const auto child = ::fork();
    require(child >= 0, "fork fd zero test");
    if (child == 0) {
        (void)::close(STDIN_FILENO);
        auto result = create_eventfd_wakeup();
        if (!std::holds_alternative<UniqueFd>(result))
            ::_exit(2);
        auto &fd = std::get<UniqueFd>(result);
        if (fd.get() != 0 || notify_eventfd(fd.get())
            || drain_eventfd(fd.get()).successful_reads != 1 || fd.close())
            ::_exit(3);
        ::_exit(0);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fd zero owned");
    std::cout << "eventfd checks=" << checks << '\n';
}
