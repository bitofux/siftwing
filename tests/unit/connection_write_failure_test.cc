/*
 * PROJECT : SIFTWING
 * FILE    : connection_write_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 测试专用send/close与分配注入验证精确前缀、预算、首因及强保证
 */
#include "siftwing/net/connection.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0, send_calls = 0, close_calls = 0;
std::array<int, 32> actions{};
std::array<std::size_t, 32> capacities{};
std::size_t cursor = 0, action_count = 0, allocation_count = 0, fail_allocation = 0;
int target_fd = -1, observed_flags = 0;
bool allocation_scope = false, close_failure = false;
void require(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        std::cerr << "connection write injection failed: " << message << '\n';
        std::exit(1);
    }
}
void plan(std::initializer_list<int> schedule) {
    require(schedule.size() <= actions.size(), "finite send schedule");
    action_count = schedule.size();
    cursor = 0;
    send_calls = 0;
    std::copy(schedule.begin(), schedule.end(), actions.begin());
}
struct Pair {
    UniqueFd server;
    UniqueFd peer;
};
Pair pair() {
    int fd[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd) == 0,
        "real stream for failure policy");
    return {UniqueFd(fd[0]), UniqueFd(fd[1])};
}
// 手写固定请求/响应，期望字节独立于生产encoder；请求ID为1。
std::string wire(std::uint16_t type = 3) {
    std::string bytes("SFTW\x01\x18\x00\x00\x00\x00\x00\x00", 12);
    bytes[6] = static_cast<char>(type >> 8);
    bytes[7] = static_cast<char>(type & 255U);
    bytes.append("\x00\x00\x00\x02", 4);
    bytes.append(7, '\0');
    bytes.push_back(1);
    bytes += "{}";
    return bytes;
}
void ready(Connection &connection, int peer, bool cached_next = false) {
    auto bytes = wire();
    if (cached_next)
        bytes += wire();
    require(::send(peer, bytes.data(), bytes.size(), MSG_NOSIGNAL)
                == static_cast<ssize_t>(bytes.size()),
        "request fixture sent");
    require(connection.read_request().stop == ReadStop::request, "request context");
    target_fd = connection.get();
    plan({});
}
const Frame response{MessageType::search_response, 0, 1, "{}"};
void queued(Connection &connection) {
    require(
        connection.queue_response(response).stop == QueueStop::queued, "complete response queued");
}
std::string received(int peer) {
    std::array<char, 1024> bytes{};
    const auto n = ::recv(peer, bytes.data(), bytes.size(), MSG_DONTWAIT);
    require(n > 0, "real sent prefix visible at peer");
    return {bytes.data(), static_cast<std::size_t>(n)};
}
void arm(std::size_t nth) {
    allocation_count = 0;
    fail_allocation = nth;
    allocation_scope = true;
}
bool error_is(std::error_code error, int value) {
    return error.value() == value && error.category() == std::generic_category();
}
} // namespace
// 只有本测试替换分配，生产没有hook；malloc/free配对仍可被ASan观测。
void *operator new(std::size_t size) {
    if (allocation_scope && ++allocation_count == fail_allocation)
        throw std::bad_alloc();
    if (void *memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void *memory) noexcept {
    std::free(memory);
}
void operator delete[](void *memory) noexcept {
    std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}
extern "C" {
ssize_t __real_send(int, const void *, std::size_t, int);
int __real_close(int);
ssize_t __wrap_send(int fd, const void *bytes, std::size_t size, int flags) {
    if (fd != target_fd)
        return __real_send(fd, bytes, size, flags);
    if (send_calls < capacities.size())
        capacities[send_calls] = size;
    ++send_calls;
    observed_flags = flags;
    if (cursor < action_count) {
        const int action = actions[cursor++];
        if (action < 0) {
            errno = -action;
            return -1;
        }
        if (action == 0)
            return 0;
        // 正值仍真正发送指定前缀，期望由对端字节独立核对，不伪造发送成功。
        return __real_send(fd, bytes, std::min(size, static_cast<std::size_t>(action)), flags);
    }
    return __real_send(fd, bytes, size, flags);
}
int __wrap_close(int fd) {
    ++close_calls;
    const auto result = __real_close(fd);
    if (fd == target_fd && close_failure) {
        errno = EINTR;
        return -1;
    }
    return result;
}
}
int main() {
    static_assert(
        noexcept(std::declval<Connection &>().flush_output()), "send path must not throw");
    std::size_t allocation_points = 0;
    {
        auto sockets = pair();
        Connection connection(std::move(sockets.server), 256);
        target_fd = connection.get();
        plan({});
        require(connection.flush_output(0, 0).stop == WriteStop::drained && send_calls == 0,
            "empty output zero syscall before invalid budget");
        ready(connection, sockets.peer.get());
        queued(connection);
        require(connection.flush_output(0, 1).stop == WriteStop::invalid_budget
                    && connection.flush_output(1, 0).stop == WriteStop::invalid_budget
                    && send_calls == 0,
            "zero budgets do not call send");
        plan({-EINTR, -EINTR, -EINTR});
        auto result = connection.flush_output(100, 2);
        require(result.stop == WriteStop::budget_exhausted && result.attempts == 2
                    && result.bytes == 0 && !result.error && connection.output_bytes() == 26,
            "finite interruption storm bounded");
        result = connection.flush_output(100, 2);
        require(result.stop == WriteStop::drained && result.attempts == 2 && result.bytes == 26
                    && observed_flags == (MSG_DONTWAIT | MSG_NOSIGNAL),
            "interruption recovery and exact send flags");
        require(received(sockets.peer.get()) == wire(0x8003), "no loss after interrupted attempts");
    }
    {
        auto sockets = pair();
        Connection connection(std::move(sockets.server), 256);
        ready(connection, sockets.peer.get(), true);
        queued(connection);
        plan({3, -EINTR, 5, -EAGAIN});
        auto result = connection.flush_output(7, 6);
        require(result.stop == WriteStop::budget_exhausted && result.attempts == 3
                    && result.bytes == 7 && capacities[0] == 7 && capacities[1] == 4
                    && capacities[2] == 4 && connection.output_bytes() == 19
                    && connection.buffered_bytes() == 26,
            "short writes advance only actual prefix and cap syscall by remaining budget");
        result = connection.flush_output(100, 1);
        require(result.stop == WriteStop::would_block && error_is(result.error, EAGAIN)
                    && result.attempts == 1 && result.bytes == 0 && connection.output_bytes() == 19
                    && !connection.finish_request(),
            "temporary refusal preserves suffix and gate");
        result = connection.flush_output(19, 1);
        require(result.stop == WriteStop::drained && result.attempts == 1 && result.bytes == 19
                    && !connection.needs_write(),
            "last send drain wins over both budgets");
        require(received(sockets.peer.get()) == wire(0x8003) && connection.finish_request(),
            "real prefix concatenation exactly once");
        require(connection.read_request(1, 1).request.has_value(),
            "cached input survives output operations");
    }
    // 错误注入只证明本地政策，不声称每errno在真实TCP上都可稳定制造。
    for (const auto error : {EPIPE, ECONNRESET, ENOBUFS, EBADF, EIO, EINVAL, ENOMEM}) {
        auto sockets = pair();
        Connection connection(std::move(sockets.server), 256);
        ready(connection, sockets.peer.get(), true);
        queued(connection);
        const auto closed_before = close_calls;
        const int fd = connection.get();
        close_failure = true;
        plan({5, -error});
        const auto result = connection.flush_output();
        close_failure = false;
        require(result.stop == WriteStop::io_error && result.bytes == 5 && result.attempts == 2
                    && error_is(result.error, error) && close_calls == closed_before + 1
                    && connection.get() == -1 && connection.output_bytes() == 0
                    && connection.buffered_bytes() == 0 && !connection.request_pending()
                    && !connection.needs_write(),
            "send first cause survives close EINTR and drops both buffers");
        require(received(sockets.peer.get()) == wire(0x8003).substr(0, 5),
            "only successful prefix was sent");
        require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "fd really closed");
        const auto calls = send_calls;
        const auto again = connection.flush_output(0, 0);
        require(again.stop == WriteStop::io_error && error_is(again.error, error)
                    && again.bytes == 0 && again.attempts == 0 && !connection.close()
                    && send_calls == calls && close_calls == closed_before + 1,
            "repeated terminal query no send or second close");
        const auto read = connection.read_request();
        require(read.stop == ReadStop::io_error && error_is(read.error, error),
            "read sees shared terminal cause");
    }
    {
        auto sockets = pair();
        Connection connection(std::move(sockets.server));
        ready(connection, sockets.peer.get());
        queued(connection);
        plan({0});
        const auto before = close_calls;
        const auto result = connection.flush_output();
        require(result.stop == WriteStop::io_error && error_is(result.error, EIO)
                    && result.bytes == 0 && result.attempts == 1 && close_calls == before + 1,
            "nonempty send zero cannot busy loop or masquerade as drain");
    }
    const Frame large{
        MessageType::search_response, 0, 1, "{\"pad\":\"" + std::string(256, 'x') + "\"}"};
    {
        auto sockets = pair();
        Connection connection(std::move(sockets.server), 512);
        ready(connection, sockets.peer.get());
        arm(0);
        const auto result = connection.queue_response(large);
        allocation_points = allocation_count;
        allocation_scope = false;
        require(result.stop == QueueStop::queued && allocation_points >= 2,
            "measure real encoding and output allocation sites");
        arm(1);
        const auto written = connection.flush_output();
        const auto allocations = allocation_count;
        allocation_scope = false;
        require(written.stop == WriteStop::drained && allocations == 0, "flush allocates nothing");
    }
    // 对测得的每个真实分配点失败：没有send，因此可保留请求和输出状态后重试。
    for (std::size_t nth = 1; nth <= allocation_points; ++nth) {
        auto sockets = pair();
        Connection connection(std::move(sockets.server), 512);
        ready(connection, sockets.peer.get(), true);
        const int fd = connection.get();
        const auto before = close_calls;
        bool threw = false;
        arm(nth);
        try {
            (void)connection.queue_response(large);
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        allocation_scope = false;
        require(threw && allocation_count == nth && connection.get() == fd
                    && connection.request_pending() && connection.output_bytes() == 0
                    && !connection.needs_write() && connection.buffered_bytes() == 26
                    && send_calls == 0 && close_calls == before,
            "each encoding/append allocation failure has strong guarantee");
        require(connection.queue_response(large).stop == QueueStop::queued
                    && connection.flush_output().stop == WriteStop::drained
                    && connection.finish_request(),
            "same logical response can be retried after allocation failure");
    }
    std::cout << "checks=" << checks << " allocation_points=" << allocation_points
              << " failures=0\n";
}
