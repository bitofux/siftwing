/*
 * PROJECT : SIFTWING
 * FILE    : connection_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 测试专用recv/close与分配注入验证预算、错误首因、门禁和异常清理
 */
#include "siftwing/net/connection.hpp"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0, recv_calls = 0, close_calls = 0;
std::array<int, 32> schedule{};
std::size_t cursor = 0, schedule_size = 0, allocation_count = 0, fail_allocation = 0;
bool allocation_scope = false, close_eintr = false;
int observed_flags = 0, observed_fd = -1;
std::size_t observed_capacity = 0;
void require(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        std::cerr << "connection injection failed: " << message << '\n';
        std::exit(1);
    }
}
bool error_is(std::error_code error, int reason) {
    return error.value() == reason && error.category() == std::generic_category();
}
void plan(std::initializer_list<int> errors) {
    require(errors.size() <= schedule.size(), "finite error fixture");
    cursor = 0;
    schedule_size = errors.size();
    std::size_t i = 0;
    for (const auto error : errors)
        schedule[i++] = error;
}
struct Pair {
    UniqueFd reader;
    UniqueFd writer;
};
Pair pair() {
    int fd[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd) == 0,
        "real stream for injection");
    return {UniqueFd(fd[0]), UniqueFd(fd[1])};
}
// 独立固定24字节头；长Body用于同时经过Buffer、Frame副本和JSON SAX的真实分配。
std::string wire(std::string_view body = "{}") {
    std::string bytes("SFTW\x01\x18\x00\x03\x00\x00\x00\x00", 12);
    const auto size = static_cast<std::uint32_t>(body.size());
    for (unsigned int i = 0; i < 4; ++i)
        bytes.push_back(static_cast<char>((size >> ((3U - i) * 8U)) & 255U));
    bytes.append(7, '\0');
    bytes.push_back(1);
    bytes.append(body);
    return bytes;
}
void sent(int fd, std::string_view bytes) {
    require(
        ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size()),
        "small fixture sent");
}
void arm(std::size_t nth) {
    allocation_count = 0;
    fail_allocation = nth;
    allocation_scope = true;
}
} // namespace
// 只有这个测试可替换new；生产目标没有hook。malloc/free保持ASan可观测的分配配对。
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
ssize_t __real_recv(int, void *, std::size_t, int);
int __real_close(int);
ssize_t __wrap_recv(int fd, void *destination, std::size_t capacity, int flags) {
    ++recv_calls;
    observed_fd = fd;
    observed_flags = flags;
    observed_capacity = capacity;
    if (cursor < schedule_size) {
        const int error = schedule[cursor++];
        if (error) {
            errno = error;
            return -1;
        }
    }
    return __real_recv(fd, destination, capacity, flags);
}
int __wrap_close(int fd) {
    ++close_calls;
    const int result = __real_close(fd);
    if (close_eintr) {
        errno = EINTR;
        return -1;
    }
    return result;
}
}
int main() {
    // 门禁与零预算均零调用；已缓存完整下一帧同样不调用内核。
    {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader), 128);
        const auto initial_calls = recv_calls;
        for (const auto budgets : {std::array<std::size_t, 2>{0, 1}, {1, 0}, {0, 0}}) {
            auto result = connection.read_request(budgets[0], budgets[1]);
            require(result.stop == ReadStop::invalid_budget && error_is(result.error, EINVAL)
                        && result.attempts == 0 && recv_calls == initial_calls,
                "invalid budgets zero syscall");
        }
        sent(sockets.writer.get(), wire() + wire());
        auto first = connection.read_request();
        require(first.stop == ReadStop::request && first.attempts == 1 && first.bytes == 52,
            "one real recv for coalesced fixture");
        require(observed_fd == connection.get() && observed_flags == MSG_DONTWAIT
                    && observed_capacity > 0 && observed_capacity <= 4096,
            "bounded nonempty DONTWAIT recv");
        const auto before = recv_calls;
        require(connection.read_request().stop == ReadStop::request_pending && recv_calls == before,
            "in flight gate zero recv");
        require(connection.finish_request(), "first reply finished");
        auto second = connection.read_request();
        require(second.stop == ReadStop::request && second.attempts == 0 && second.bytes == 0
                    && recv_calls == before,
            "cached next frame zero recv");
    }
    // 连续EINTR包含在预算；最后一次成功仍解码，耗尽不能误报EAGAIN/队列空。
    for (const std::size_t budget : {1U, 2U, 4U}) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader));
        plan({EINTR, EINTR, EINTR, EINTR, 0});
        const auto before = recv_calls;
        auto result = connection.read_request(4096, budget);
        require(result.stop == ReadStop::budget_exhausted && !result.error
                    && result.attempts == budget && result.bytes == 0
                    && recv_calls - before == budget && connection.get() >= 0,
            "error storm finite attempts");
    }
    {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader));
        sent(sockets.writer.get(), wire());
        plan({EINTR, 0});
        auto result = connection.read_request(26, 2);
        require(result.stop == ReadStop::request && result.attempts == 2 && result.bytes == 26,
            "request wins last attempt after EINTR");
    }
    for (const int error : {EAGAIN, EWOULDBLOCK}) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader));
        sent(sockets.writer.get(), "SFTW");
        plan({0, error});
        auto result = connection.read_request();
        require(result.stop == ReadStop::would_block && result.attempts == 2 && result.bytes == 4
                    && error_is(result.error, error) && connection.buffered_bytes() == 4
                    && connection.get() >= 0,
            "EAGAIN preserves partial input and errno");
    }
    for (const int error : {ECONNRESET, ETIMEDOUT, ENOMEM, ENOBUFS, EBADF, EIO, ENOTCONN}) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader));
        const int fd = connection.get();
        sent(sockets.writer.get(), "SFTW");
        plan({0, EINTR, error});
        const auto before = close_calls;
        close_eintr = true;
        auto result = connection.read_request();
        close_eintr = false;
        require(result.stop == ReadStop::io_error && result.bytes == 4 && result.attempts == 3
                    && error_is(result.error, error) && connection.get() == -1
                    && connection.buffered_bytes() == 0 && close_calls == before + 1,
            "terminal IO closes once, close EINTR cannot replace original errno");
        require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "real closed injected terminal fd");
        const auto calls = recv_calls;
        auto again = connection.read_request();
        require(again.stop == ReadStop::io_error && error_is(again.error, error)
                    && again.attempts == 0 && again.bytes == 0 && recv_calls == calls
                    && !connection.finish_request() && !connection.close()
                    && close_calls == before + 1,
            "terminal diagnostic idempotent and no future recv");
    }
    plan({});
    // 构造Buffer分配失败不得接管调用方fd；失败后能构造一个正常连接。
    {
        auto sockets = pair();
        const int fd = sockets.reader.get();
        const auto before = close_calls;
        bool threw = false;
        arm(1);
        try {
            Connection failed(std::move(sockets.reader), 128, 152);
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        allocation_scope = false;
        require(threw && sockets.reader.get() == fd && ::fcntl(fd, F_GETFD) >= 0
                    && close_calls == before,
            "construction allocation failure retains source");
        Connection good(std::move(sockets.reader), 128, 152);
        require(good.get() == fd && !sockets.reader,
            "successful construction transfers after allocation");
    }
    const auto bytes = wire("{\"pad\":\"" + std::string(96, 'x') + "\"}");
    std::size_t allocation_points = 0;
    {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader), 256);
        sent(sockets.writer.get(), bytes);
        arm(0);
        auto result = connection.read_request();
        allocation_scope = false;
        allocation_points = allocation_count;
        require(result.stop == ReadStop::request && allocation_points >= 3,
            "observed Buffer Frame and JSON allocations");
    }
    // 对此路径实际每个分配点独立抛bad_alloc，覆盖recv后append、Body复制和SAX分配。
    for (std::size_t nth = 1; nth <= allocation_points; ++nth) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader), 256);
        const int fd = connection.get();
        sent(sockets.writer.get(), bytes);
        const auto before = close_calls;
        bool threw = false;
        arm(nth);
        try {
            (void)connection.read_request();
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        allocation_scope = false;
        require(threw && allocation_count == nth && connection.get() == -1
                    && !connection.request_pending() && connection.buffered_bytes() == 0
                    && close_calls == before + 1,
            "every allocation failure closes then propagates");
        require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "allocation failure fd truly closed");
        const auto calls = recv_calls;
        require(connection.read_request().stop == ReadStop::local_failure && recv_calls == calls,
            "no retry of stream after consumed bytes allocation failure");
    }
    // 分配异常发生在解码已经缓存的第二帧时，也必须终止；不能静默吞掉一帧继续读。
    {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader), 512, 536);
        sent(sockets.writer.get(), wire() + bytes);
        auto first = connection.read_request();
        require(first.stop == ReadStop::request && connection.finish_request(), "first reply done");
        const auto before = recv_calls;
        bool threw = false;
        arm(1);
        try {
            (void)connection.read_request();
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        allocation_scope = false;
        require(
            threw && recv_calls == before && connection.get() == -1 && first.request->body == "{}",
            "cached decode failure no recv, old owning request remains");
    }
    std::cout << "checks=" << checks << " allocation_points=" << allocation_points
              << " failures=0\n";
}
