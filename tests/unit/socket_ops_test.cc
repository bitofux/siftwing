/*
 * PROJECT : SIFTWING
 * FILE    : socket_ops_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 真实Linux fd/socket验证拥有、非阻塞、短读写、半关闭和SIGPIPE
 */
#include "siftwing/net/socket_ops.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
std::size_t checks = 0;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "socket check failed: " << label << " errno=" << errno << '\n';
        std::exit(1);
    }
}
bool open(int fd) {
    return ::fcntl(fd, F_GETFD) >= 0;
}
std::size_t fd_count() {
    DIR *directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "proc fd directory");
    std::size_t count = 0;
    while (const auto *entry = ::readdir(directory)) {
        if (entry->d_name[0] != '.')
            ++count;
    }
    require(::closedir(directory) == 0, "close proc directory");
    return count;
}
std::array<UniqueFd, 2> pair() {
    int descriptors[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) == 0,
        "real blocking socketpair");
    return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}
void wait(int fd, short events) {
    pollfd item{fd, events, 0};
    require(::poll(&item, 1, 2000) == 1 && (item.revents & events) != 0, "bounded fixture poll");
}
void ownership() {
    static_assert(!std::is_copy_constructible_v<UniqueFd> && !std::is_copy_assignable_v<UniqueFd>);
    static_assert(std::is_nothrow_move_constructible_v<UniqueFd>
                  && std::is_nothrow_move_assignable_v<UniqueFd>
                  && std::is_nothrow_destructible_v<UniqueFd>);
    static_assert(std::is_nothrow_move_constructible_v<SocketResult>);
    const auto baseline = fd_count();
    {
        UniqueFd empty(-23);
        require(!empty && empty.get() == -1 && empty.release() == -1 && !empty.close(),
            "negative values normalize empty");
        UniqueFd a(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        require(static_cast<bool>(a), "fixture fd opens");
        const int first = a.get();
        UniqueFd b(std::move(a));
        require(!a && a.get() == -1 && b.get() == first && open(first), "move transfers");
        UniqueFd c(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        const int retired = c.get();
        c = std::move(b);
        require(!b && c.get() == first && !open(retired) && open(first),
            "move assignment closes old resource");
        auto *alias = &c;
        c = std::move(*alias);
        require(c.get() == first && open(first), "self move preserves");
        const int released = c.release();
        require(!c && open(released), "release does not close");
        UniqueFd reclaimed(released);
        errno = EDOM;
        require(!reclaimed.close() && !reclaimed && errno == EDOM && !open(first),
            "explicit close clears owner and preserves errno");
        require(!reclaimed.close(), "repeated empty close succeeds");
    }
    int retired = -1;
    try {
        UniqueFd guard(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        retired = guard.get();
        throw std::runtime_error("fixture unwind");
    } catch (const std::runtime_error &) {
        require(!open(retired), "exception unwinding closes fd");
    }
    // 数字复用证明关闭后的拥有者不能再关闭该数字所指的新资源。
    int reused = -1;
    {
        UniqueFd guard(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        const int number = guard.get();
        require(!guard.close(), "close before reuse");
        reused = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        require(reused == number, "Linux reuses lowest available fd");
    }
    {
        UniqueFd guard(reused);
        require(open(guard.get()), "empty old destructor preserves reused fd");
    }
    require(fd_count() == baseline, "ownership cases do not leak");
}
void creation() {
    for (const auto family : {IpFamily::v4, IpFamily::v6}) {
        auto result = open_tcp_socket(family);
        require(std::holds_alternative<UniqueFd>(result), "IPv4/IPv6 TCP socket creation");
        auto socket = std::get<UniqueFd>(std::move(result));
        require((::fcntl(socket.get(), F_GETFL) & O_NONBLOCK) != 0, "atomic nonblocking flag");
        require((::fcntl(socket.get(), F_GETFD) & FD_CLOEXEC) != 0, "atomic close-on-exec flag");
        int type = 0;
        socklen_t length = sizeof type;
        require(::getsockopt(socket.get(), SOL_SOCKET, SO_TYPE, &type, &length) == 0
                    && type == SOCK_STREAM,
            "stream socket type");
    }
    auto invalid = open_tcp_socket(static_cast<IpFamily>(99));
    require(std::get<std::error_code>(invalid) == std::error_code(EINVAL, std::generic_category()),
        "invalid address family is explicit error");
}
void io() {
    auto sockets = pair();
    auto &a = sockets[0];
    auto &b = sockets[1];
    std::array<char, 8192> storage{};
    const int flags = ::fcntl(a.get(), F_GETFL);
    require((flags & O_NONBLOCK) == 0, "fixture is blocking");
    const auto idle = receive_some(a.get(), storage.data(), storage.size());
    require(idle.state == IoState::would_block && idle.bytes == 0
                && (idle.error.value() == EAGAIN || idle.error.value() == EWOULDBLOCK),
        "MSG_DONTWAIT returns receive would-block");
    require(::fcntl(a.get(), F_GETFL) == flags, "per-call flags do not alter descriptor");
    const auto zero = receive_some(a.get(), nullptr, 0);
    require(zero.state == IoState::progress && zero.bytes == 0 && !zero.error,
        "zero receive is not EOF");
    require(send_some(a.get(), {}).state == IoState::progress, "empty send is no operation");
    require(receive_some(a.get(), nullptr, 1).error.value() == EINVAL, "nonempty null rejected");
    require(
        receive_some(a.get(), storage.data(), std::numeric_limits<std::size_t>::max()).error.value()
            == EINVAL,
        "oversized receive rejected");
    require(send_some(-1, "x").error.value() == EBADF
                && receive_some(-1, storage.data(), 1).error.value() == EBADF,
        "negative fd rejected");
    UniqueFd regular(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    require(send_some(regular.get(), "x").error.value() == ENOTSOCK
                && receive_some(regular.get(), storage.data(), 1).error.value() == ENOTSOCK,
        "kernel nonsocket error retained");
    const std::string payload("A\0B\xff"
                              "C",
        5);
    require(send_some(a.get(), payload).bytes == payload.size(), "binary send accepted");
    auto first = receive_some(b.get(), storage.data(), 2);
    require(first.state == IoState::progress && first.bytes == 2
                && std::string_view(storage.data(), first.bytes)
                       == std::string_view(payload).substr(0, 2),
        "short receive reports exact prefix");
    auto second = receive_some(b.get(), storage.data(), storage.size());
    require(second.bytes == 3
                && std::string_view(storage.data(), 3) == std::string_view(payload).substr(2),
        "remaining bytes retain order");
    require(send_some(a.get(), "pending").bytes == 7 && ::shutdown(a.get(), SHUT_WR) == 0,
        "fixture half-close after pending bytes");
    require(receive_some(b.get(), storage.data(), storage.size()).bytes == 7,
        "pending data delivered before EOF");
    require(receive_some(b.get(), storage.data(), storage.size()).state == IoState::eof,
        "half-close exposes receive EOF");
    require(
        send_some(b.get(), "ok").bytes == 2 && receive_some(a.get(), storage.data(), 2).bytes == 2,
        "read direction survives write half-close");
}
void partial_send() {
    auto sockets = pair();
    const int budget = 4096;
    require(::setsockopt(sockets[0].get(), SOL_SOCKET, SO_SNDBUF, &budget, sizeof budget) == 0,
        "small fixture send buffer");
    const std::string bytes(65536, 'x');
    std::size_t total = 0;
    bool blocked = false, partial = false;
    for (unsigned int i = 0; i < 64; ++i) {
        const auto result = send_some(sockets[0].get(), bytes);
        if (result.state == IoState::would_block) {
            blocked = true;
            break;
        }
        require(
            result.state == IoState::progress && result.bytes > 0 && result.bytes <= bytes.size(),
            "send progress reports bounded positive amount");
        partial = partial || result.bytes < bytes.size();
        total += result.bytes;
    }
    require(blocked && partial && total > 0, "real short send then EAGAIN under pressure");
    std::size_t drained = 0;
    std::array<char, 4096> data{};
    for (unsigned int i = 0; i < 1024; ++i) {
        const auto result = receive_some(sockets[1].get(), data.data(), data.size());
        if (result.state == IoState::would_block)
            break;
        require(result.state == IoState::progress && result.bytes > 0, "drain exact send progress");
        require(std::string_view(data.data(), result.bytes) == std::string(result.bytes, 'x'),
            "kernel contains only accepted prefix bytes");
        drained += result.bytes;
    }
    require(drained == total, "short-send accounting equals receiver bytes");
}
void tcp_loopback() {
    // bind/listen/connect/accept4仅组装测试夹具，不引入生产Listener或Connection。
    auto listener_result = open_tcp_socket();
    auto listener = std::get<UniqueFd>(std::move(listener_result));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(
        ::bind(listener.get(), reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0
            && ::listen(listener.get(), 1) == 0,
        "loopback fixture listen");
    socklen_t length = sizeof address;
    require(::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address), &length) == 0,
        "ephemeral loopback port");
    auto client_result = open_tcp_socket();
    auto client = std::get<UniqueFd>(std::move(client_result));
    const int connected =
        ::connect(client.get(), reinterpret_cast<const sockaddr *>(&address), sizeof address);
    require(connected == 0 || errno == EINPROGRESS, "fixture nonblocking connect");
    wait(client.get(), POLLOUT);
    int error = -1;
    length = sizeof error;
    require(::getsockopt(client.get(), SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0,
        "fixture connect completed");
    wait(listener.get(), POLLIN);
    UniqueFd peer(::accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    require(static_cast<bool>(peer), "fixture accept connected socket");
    const std::string_view payload = "loopback";
    std::size_t sent = 0;
    for (unsigned int attempt = 0; sent < payload.size() && attempt < 16; ++attempt) {
        wait(client.get(), POLLOUT);
        const auto output = send_some(client.get(), payload.substr(sent));
        require(output.state == IoState::progress && output.bytes > 0, "real TCP send progress");
        sent += output.bytes;
    }
    require(sent == payload.size(), "bounded fixture sends full payload");
    std::array<char, 16> bytes{};
    std::size_t received = 0;
    for (unsigned int attempt = 0; received < payload.size() && attempt < 16; ++attempt) {
        wait(peer.get(), POLLIN);
        const auto input =
            receive_some(peer.get(), bytes.data() + received, payload.size() - received);
        require(input.state == IoState::progress && input.bytes > 0, "real TCP receive progress");
        received += input.bytes;
    }
    require(received == payload.size() && std::string_view(bytes.data(), received) == payload,
        "real TCP bytes arrived with no assumed recv boundary");
}
void children() {
    // SIGPIPE默认动作在隔离子进程验证，活动测试进程和生产库均不修改全局信号策略。
    for (unsigned int scenario = 0; scenario < 2; ++scenario) {
        const pid_t pid = ::fork();
        require(pid >= 0, "fork isolated signal/fd0 case");
        if (pid == 0) {
            if (scenario == 0) {
                struct sigaction disposition {};
                disposition.sa_handler = SIG_DFL;
                ::sigemptyset(&disposition.sa_mask);
                require(::sigaction(SIGPIPE, &disposition, nullptr) == 0, "child default SIGPIPE");
                // 子进程也解除继承屏蔽，保证缺少MSG_NOSIGNAL会受到默认SIGPIPE动作影响。
                sigset_t unblocked{};
                require(::sigemptyset(&unblocked) == 0 && ::sigaddset(&unblocked, SIGPIPE) == 0
                            && ::sigprocmask(SIG_UNBLOCK, &unblocked, nullptr) == 0,
                    "child unblocks SIGPIPE");
                auto sockets = pair();
                require(!sockets[1].close(), "child peer closes");
                const auto result = send_some(sockets[0].get(), "x");
                require(result.state == IoState::error && result.error.value() == EPIPE,
                    "MSG_NOSIGNAL survives default SIGPIPE and reports EPIPE");
                require(::sigaction(SIGPIPE, nullptr, &disposition) == 0
                            && disposition.sa_handler == SIG_DFL,
                    "send leaves signal policy unchanged");
            } else {
                (void)::close(0);
                auto result = open_tcp_socket();
                auto owner = std::get<UniqueFd>(std::move(result));
                require(owner.get() == 0 && static_cast<bool>(owner), "fd0 is owned");
                require(!owner.close() && !open(0), "fd0 closes");
            }
            ::_exit(0);
        }
        int status = 0;
        require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "isolated child exited normally");
    }
}
} // namespace
int main() {
    const auto baseline = fd_count();
    ownership();
    creation();
    io();
    partial_send();
    tcp_loopback();
    children();
    require(fd_count() == baseline, "all socket cases preserve fd count");
    std::cout << "parent_checks=" << checks << " isolated_children=2 failures=0\n";
}
