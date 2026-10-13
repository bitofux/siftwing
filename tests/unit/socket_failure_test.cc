/*
 * PROJECT : SIFTWING
 * FILE    : socket_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 仅测试链接器wrap注入系统失败和短IO，证明单次调用与close不重试
 */
#include "siftwing/net/socket_ops.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
int socket_error = 0, recv_error = 0, send_error = 0, close_error = 0;
unsigned int socket_calls = 0, recv_calls = 0, send_calls = 0, close_calls = 0;
int observed_domain = 0, observed_type = 0, observed_protocol = 0;
int recv_flags = 0, send_flags = 0, recycled_fd = -1;
bool force_short_recv = false, force_short_send = false, recycle_on_close = false;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "socket injection failed: " << label << '\n';
        std::exit(1);
    }
}
} // namespace
extern "C" {
int __real_socket(int, int, int);
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __real_send(int, const void *, std::size_t, int);
int __real_close(int);
int __wrap_socket(int domain, int type, int protocol) {
    ++socket_calls;
    observed_domain = domain;
    observed_type = type;
    observed_protocol = protocol;
    if (socket_error) {
        errno = socket_error;
        return -1;
    }
    return __real_socket(domain, type, protocol);
}
ssize_t __wrap_recv(int fd, void *data, std::size_t size, int flags) {
    ++recv_calls;
    recv_flags = flags;
    if (recv_error) {
        errno = recv_error;
        return -1;
    }
    // 真正读取更短范围，使返回量与内核消费一致，不能只篡改返回数字。
    return __real_recv(fd, data, force_short_recv && size > 1 ? 1 : size, flags);
}
ssize_t __wrap_send(int fd, const void *data, std::size_t size, int flags) {
    ++send_calls;
    send_flags = flags;
    if (send_error) {
        errno = send_error;
        return -1;
    }
    return __real_send(fd, data, force_short_send && size > 1 ? 1 : size, flags);
}
int __wrap_close(int fd) {
    ++close_calls;
    const int result = __real_close(fd);
    if (close_error) {
        // 模拟Linux释放槽后报告错误；期间打开新资源复用数字，验证不会再次close。
        if (recycle_on_close)
            recycled_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        errno = close_error;
        return -1;
    }
    return result;
}
}
int main() {
    using namespace siftwing::net;
    for (const int reason : {EMFILE, ENFILE, EINTR, EAFNOSUPPORT}) {
        socket_error = reason;
        const auto before = socket_calls;
        auto result = open_tcp_socket();
        require(std::holds_alternative<std::error_code>(result)
                    && std::get<std::error_code>(result).value() == reason
                    && socket_calls == before + 1,
            "socket failure retains errno without retry");
        require(observed_domain == AF_INET && observed_protocol == IPPROTO_TCP
                    && observed_type == (SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC),
            "socket uses atomic flags in single call");
    }
    socket_error = 0;
    const auto before_invalid = socket_calls;
    require(std::get<std::error_code>(open_tcp_socket(static_cast<IpFamily>(99))).value() == EINVAL
                && socket_calls == before_invalid,
        "invalid family makes no socket call");
    for (const int reason : {EINTR, EIO}) {
        close_error = reason;
        recycle_on_close = true;
        const auto before = close_calls;
        int retired = -1;
        {
            UniqueFd owned(::open("/dev/null", O_RDONLY | O_CLOEXEC));
            retired = owned.get();
            errno = EDOM;
            const auto error = owned.close();
            require(error.value() == reason && error.category() == std::generic_category() && !owned
                        && errno == EDOM,
                "close clears ownership and captures error");
            require(close_calls == before + 1 && recycled_fd == retired,
                "close error performed once and fd number reused");
            require(
                !owned.close() && close_calls == before + 1, "empty explicit close never retries");
        }
        require(close_calls == before + 1 && ::fcntl(recycled_fd, F_GETFD) >= 0,
            "destructor preserves recycled resource after close error");
        close_error = 0;
        recycle_on_close = false;
        require(__real_close(recycled_fd) == 0, "release recycled fixture");
    }
    // 析构和移动赋值不能抛出或重复close，即使内核报告错误。
    const auto before_cleanup = close_calls;
    {
        UniqueFd first(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        UniqueFd second(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        close_error = EINTR;
        errno = ERANGE;
        first = std::move(second);
        require(!second && first && errno == ERANGE && close_calls == before_cleanup + 1,
            "move assignment ignores cleanup error, transfers and preserves errno");
        errno = EDOM;
    }
    require(close_calls == before_cleanup + 2 && errno == EDOM,
        "destructor closes once and preserves errno");
    close_error = 0;
    int fds[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0, "IO injection pair");
    UniqueFd a(fds[0]), b(fds[1]);
    char data[8] = {'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q'};
    for (const int reason : {EINTR, EAGAIN, EWOULDBLOCK, ECONNRESET, EPIPE, EIO}) {
        const auto expected = reason == EINTR                               ? IoState::interrupted
                              : (reason == EAGAIN || reason == EWOULDBLOCK) ? IoState::would_block
                                                                            : IoState::error;
        recv_error = reason;
        auto before = recv_calls;
        const auto input = receive_some(a.get(), data, sizeof data);
        require(input.state == expected && input.bytes == 0 && input.error.value() == reason
                    && recv_calls == before + 1 && data[0] == 'q',
            "recv failure state, no retry or write");
        require(recv_flags == MSG_DONTWAIT, "recv per-call nonblocking flag");
        send_error = reason;
        before = send_calls;
        const auto output = send_some(a.get(), "abcdef");
        require(output.state == expected && output.bytes == 0 && output.error.value() == reason
                    && send_calls == before + 1,
            "send failure state and exactly one call");
        require(
            send_flags == (MSG_DONTWAIT | MSG_NOSIGNAL), "send flags suppress only this SIGPIPE");
    }
    recv_error = send_error = 0;
    const auto r_before = recv_calls, s_before = send_calls;
    require(receive_some(a.get(), nullptr, 0).state == IoState::progress
                && send_some(a.get(), {}).state == IoState::progress && recv_calls == r_before
                && send_calls == s_before,
        "empty requests do not call kernel");
    force_short_send = true;
    const auto sent = send_some(a.get(), "abcdef");
    require(sent.state == IoState::progress && sent.bytes == 1 && send_calls == s_before + 1,
        "short send returns once without hidden completion");
    force_short_send = false;
    require(receive_some(b.get(), data, sizeof data).bytes == 1 && data[0] == 'a',
        "only actually accepted short-send byte arrives");
    require(send_some(a.get(), "bcd").bytes == 3, "fixture suffix send");
    force_short_recv = true;
    const auto before_short = recv_calls;
    require(receive_some(b.get(), data, sizeof data).bytes == 1 && data[0] == 'b'
                && recv_calls == before_short + 1,
        "short recv leaves suffix in kernel");
    force_short_recv = false;
    require(
        receive_some(b.get(), data, sizeof data).bytes == 2 && std::string_view(data, 2) == "cd",
        "suffix can be received later");
    std::cout << "checks=" << checks << " failures=0 test_only_wrap=socket,close,recv,send\n";
}
