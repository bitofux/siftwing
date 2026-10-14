/*
 * PROJECT : SIFTWING
 * FILE    : fd_handoff_queue_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 固定容量、FIFO、绕环、关闭与fd零的真实拥有者合同
 */
#include "siftwing/net/fd_handoff_queue.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "fd handoff failed: " << label << '\n';
        std::exit(1);
    }
}
UniqueFd descriptor() {
    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    require(static_cast<bool>(fd), "open real descriptor");
    return fd;
}
static_assert(!std::is_copy_constructible_v<FdHandoffQueue>);
static_assert(!std::is_copy_assignable_v<FdHandoffQueue>);
static_assert(!std::is_move_constructible_v<FdHandoffQueue>);
static_assert(!std::is_move_assignable_v<FdHandoffQueue>);
static_assert(!std::is_invocable_v<decltype(&FdHandoffQueue::try_push), FdHandoffQueue &, UniqueFd &&>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<UniqueFd>>);
} // namespace
int main() {
    bool rejected = false;
    try { FdHandoffQueue queue(0); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "zero capacity rejected");
    rejected = false;
    try { FdHandoffQueue queue(std::numeric_limits<std::size_t>::max()); }
    catch (const std::length_error &) { rejected = true; }
    require(rejected, "unrepresentable capacity rejected before allocation");
    // 独立参考为按提交顺序保存的真实fd；既检查整数顺序，也检查内核资源仍开放。
    for (const std::size_t capacity : {1U, 3U, 7U}) {
        FdHandoffQueue queue(capacity);
        require(!queue.try_pop(), "empty open queue");
        UniqueFd empty;
        require(queue.try_push(empty) == FdHandoffResult::invalid_fd, "empty source rejected");
        for (unsigned int cycle = 0; cycle < 64; ++cycle) {
            std::vector<int> expected;
            for (std::size_t i = 0; i < capacity; ++i) {
                auto source = descriptor();
                expected.push_back(source.get());
                require(queue.try_push(source) == FdHandoffResult::accepted && !source,
                    "successful commit consumes source");
            }
            auto overflow = descriptor();
            const int preserved = overflow.get();
            require(queue.try_push(overflow) == FdHandoffResult::full && overflow.get() == preserved,
                "full retains exact owner");
            require(queue.try_push(empty) == FdHandoffResult::invalid_fd, "invalid before full");
            require(queue.status().size == capacity, "bounded snapshot");
            auto first = queue.try_pop();
            require(first && first->get() == expected.front() && !first->close(), "remove FIFO head");
            expected.erase(expected.begin());
            require(queue.try_push(overflow) == FdHandoffResult::accepted && !overflow,
                "refill freed slot while remaining entries stay live");
            expected.push_back(preserved);
            for (const int fd : expected) {
                auto item = queue.try_pop();
                require(item && item->get() == fd && ::fcntl(fd, F_GETFD) >= 0, "FIFO real owner");
                require(!item->close(), "consumer closes outside queue");
                require(::fcntl(fd, F_GETFD) < 0 && errno == EBADF, "closed kernel resource");
            }
            // 部分取出后再入队，验证活跃区跨环尾；不能仅验证每轮完整清空。
            auto a = descriptor(); auto b = descriptor();
            const int last = b.get();
            require(queue.try_push(a) == FdHandoffResult::accepted, "partial wrap first");
            require(static_cast<bool>(queue.try_pop()), "partial wrap pop");
            require(queue.try_push(b) == FdHandoffResult::accepted, "partial wrap next");
            auto item = queue.try_pop();
            require(item && item->get() == last, "wrapped FIFO");
        }
        auto source = descriptor(); const int fd = source.get();
        require(queue.try_push(source) == FdHandoffResult::accepted, "entry before close");
        queue.close(); queue.close();
        const auto status = queue.status();
        require(status.closed && status.size == 1 && status.capacity == capacity, "close retains entries");
        auto late = descriptor(); const int late_fd = late.get();
        require(queue.try_push(late) == FdHandoffResult::closed && late.get() == late_fd, "closed retains source");
        require(queue.try_push(empty) == FdHandoffResult::closed, "closed before invalid");
        auto remaining = queue.try_pop();
        require(remaining && remaining->get() == fd, "drain after close");
        require(!queue.try_pop() && queue.status().closed, "closed never reopens");
    }
    // 用socket数据证明按值返回的fd可正常I/O，而非仅复制一个已失效数字。
    int pair[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair");
    UniqueFd source(pair[0]), peer(pair[1]);
    FdHandoffQueue queue(1);
    require(::send(peer.get(), "Q", 1, MSG_NOSIGNAL) == 1, "peer payload");
    require(queue.try_push(source) == FdHandoffResult::accepted, "socket commit");
    auto item = queue.try_pop(); char payload = 0;
    require(item && ::recv(item->get(), &payload, 1, 0) == 1 && payload == 'Q', "socket survives handoff");
    // 隔离子进程接管fd 0，避免改变测试驱动/CTest的stdin。
    const auto child = ::fork();
    require(child >= 0, "fork fd zero scenario");
    if (child == 0) {
        auto owned = descriptor();
        if (::dup2(owned.get(), 0) < 0) ::_exit(2);
        if (owned.get() == 0) (void)owned.release();
        UniqueFd zero(0); FdHandoffQueue local(1);
        if (local.try_push(zero) != FdHandoffResult::accepted || zero) ::_exit(3);
        auto output = local.try_pop();
        if (!output || output->get() != 0 || output->close()) ::_exit(4);
        if (::fcntl(0, F_GETFD) >= 0 || errno != EBADF) ::_exit(5);
        ::_exit(0);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fd zero is a valid transferable resource");
    std::cout << "fd handoff checks=" << checks << '\n';
}
