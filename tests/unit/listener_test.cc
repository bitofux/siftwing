/*
 * PROJECT : SIFTWING
 * FILE    : listener_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 真实loopback验证监听、原子接入、预算与fd生命周期
 */
#include "siftwing/net/listener.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool value, const char *message) {
    ++checks;
    if (!value) {
        std::cerr << "listener check failed: " << message << " errno=" << errno << '\n';
        std::exit(1);
    }
}
bool error_is(std::error_code error, int value) {
    return error.value() == value && error.category() == std::generic_category();
}
std::size_t fd_count() {
    auto *directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "proc fd enumeration");
    std::size_t count = 0;
    while (const auto *entry = ::readdir(directory))
        if (entry->d_name[0] != '.')
            ++count;
    require(::closedir(directory) == 0, "close enumeration");
    return count;
}
Listener listener(IpFamily family = IpFamily::v4, bool reuse = false) {
    auto result =
        Listener::create({family, family == IpFamily::v4 ? "127.0.0.1" : "::1", 0, 8, reuse});
    require(std::holds_alternative<Listener>(result), "create loopback listener");
    return std::move(std::get<Listener>(result));
}
void flags(int fd) {
    const int status = ::fcntl(fd, F_GETFL);
    const int descriptor = ::fcntl(fd, F_GETFD);
    require(status >= 0 && (status & O_NONBLOCK) != 0, "NONBLOCK flag");
    require(descriptor >= 0 && (descriptor & FD_CLOEXEC) != 0, "CLOEXEC flag");
}
UniqueFd connect_to(IpFamily family, std::uint16_t port) {
    auto result = open_tcp_socket(family);
    require(std::holds_alternative<UniqueFd>(result), "fixture client socket");
    auto client = std::move(std::get<UniqueFd>(result));
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(port);
    v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port);
    v6.sin6_addr = in6addr_loopback;
    const int connected =
        family == IpFamily::v4
            ? ::connect(client.get(), reinterpret_cast<sockaddr *>(&v4), sizeof(v4))
            : ::connect(client.get(), reinterpret_cast<sockaddr *>(&v6), sizeof(v6));
    require(connected == 0 || (connected == -1 && errno == EINPROGRESS), "connect starts");
    pollfd item{client.get(), POLLOUT, 0};
    require(::poll(&item, 1, 2000) == 1, "bounded client connection wait");
    int error = -1;
    socklen_t length = sizeof(error);
    require(::getsockopt(client.get(), SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0,
        "connection completed independently");
    return client;
}
int option(int fd, int level, int name) {
    int value = -1;
    socklen_t length = sizeof(value);
    require(::getsockopt(fd, level, name, &value, &length) == 0, "read real socket option");
    return value;
}
void basic(IpFamily family) {
    auto acceptor = listener(family, true);
    require(acceptor.port() != 0 && acceptor.get() >= 0, "kernel-selected port");
    flags(acceptor.get());
    require(option(acceptor.get(), SOL_SOCKET, SO_ACCEPTCONN) == 1, "real listening state");
    require(option(acceptor.get(), SOL_SOCKET, SO_REUSEADDR) == 1, "explicit reuse address");
    if (family == IpFamily::v6)
        require(option(acceptor.get(), IPPROTO_IPV6, IPV6_V6ONLY) == 1, "explicit v6-only");
    auto empty = acceptor.accept_one();
    require(std::holds_alternative<std::error_code>(empty)
                && error_is(std::get<std::error_code>(empty), EAGAIN),
        "empty nonblocking queue");
    auto client = connect_to(family, acceptor.port());
    auto result = acceptor.accept_one();
    require(std::holds_alternative<UniqueFd>(result), "real accepted fd");
    auto accepted = std::move(std::get<UniqueFd>(result));
    flags(accepted.get());
    require(option(accepted.get(), SOL_SOCKET, SO_ACCEPTCONN) == 0, "accepted socket not listener");
    const int retired = acceptor.get();
    require(!acceptor.close() && acceptor.get() == -1 && acceptor.port() == 0,
        "close empties listener");
    require(::fcntl(retired, F_GETFD) == -1 && errno == EBADF, "listening fd closed");
    // 真实字节交换独立证明已交付连接不归Listener析构/关闭所有。
    require(send_some(client.get(), "x").state == IoState::progress, "send after listener close");
    pollfd ready{accepted.get(), POLLIN, 0};
    require(::poll(&ready, 1, 2000) == 1, "bounded accepted data wait");
    char byte = 0;
    const auto read = receive_some(accepted.get(), &byte, 1);
    require(read.state == IoState::progress && read.bytes == 1 && byte == 'x',
        "accepted stream survives listener close");
}
void budget_and_lt() {
    auto acceptor = listener();
    auto created = create_epoll();
    require(std::holds_alternative<UniqueFd>(created), "fixture epoll");
    auto epoll = std::move(std::get<UniqueFd>(created));
    require(!epoll_add(epoll.get(), acceptor.get(), EPOLLIN, 77), "register listener LT");
    std::array<UniqueFd, 3> clients;
    for (auto &client : clients)
        client = connect_to(IpFamily::v4, acceptor.port());
    epoll_event event{};
    require(::epoll_wait(epoll.get(), &event, 1, 2000) == 1 && event.data.u64 == 77
                && (event.events & EPOLLIN) != 0,
        "listener queue ready");
    std::array<UniqueFd, 4> accepted;
    auto first = acceptor.accept_into(accepted.data(), accepted.size(), 1);
    require(first.stop == AcceptStop::budget_exhausted && first.attempts == 1 && first.accepted == 1
                && !first.error && accepted[0] && !accepted[1],
        "one attempt budget preserves untouched suffix");
    require(::epoll_wait(epoll.get(), &event, 1, 2000) == 1 && (event.events & EPOLLIN) != 0,
        "LT reports remaining queue after budget");
    auto second = acceptor.accept_into(accepted.data() + 1, 1, 1);
    require(second.stop == AcceptStop::output_full && second.accepted == 1 && second.attempts == 1
                && !second.error,
        "output-full wins simultaneous budget boundary");
    auto final = acceptor.accept_into(accepted.data() + 2, 2, 4);
    require(final.stop == AcceptStop::drained && final.accepted == 1 && final.attempts == 2
                && error_is(final.error, EAGAIN) && accepted[2] && !accepted[3],
        "success plus EAGAIN drain with exact prefix");
    require(::epoll_wait(epoll.get(), &event, 1, 0) == 0, "drained queue no remaining readiness");
    auto drained = acceptor.accept_into(accepted.data() + 3, 1, 3);
    require(drained.stop == AcceptStop::drained && drained.accepted == 0 && drained.attempts == 1,
        "empty batch one EAGAIN attempt");
    require(!epoll_remove(epoll.get(), acceptor.get()), "explicit DEL before close");
}
void validation_and_ownership() {
    static_assert(!std::is_copy_constructible_v<Listener> && !std::is_copy_assignable_v<Listener>);
    static_assert(std::is_nothrow_move_constructible_v<Listener>
                  && std::is_nothrow_move_assignable_v<Listener>
                  && std::is_nothrow_destructible_v<Listener>);
    static_assert(std::is_nothrow_move_constructible_v<ListenerResult>);
    Listener empty;
    require(error_is(std::get<std::error_code>(empty.accept_one()), EBADF), "empty accept EBADF");
    require(error_is(empty.accept_into(nullptr, 0, 0).error, EBADF), "empty fd priority");
    auto original = listener();
    const auto port = original.port();
    const int original_fd = original.get();
    auto duplicate = Listener::create({IpFamily::v4, "127.0.0.1", port, 8, false});
    require(std::holds_alternative<std::error_code>(duplicate)
                && error_is(std::get<std::error_code>(duplicate), EADDRINUSE),
        "real duplicate address refused");
    Listener moved(std::move(original));
    require(original.get() == -1 && original.port() == 0 && moved.get() == original_fd
                && moved.port() == port,
        "move resource and visible endpoint");
    auto target = listener();
    const int retired = target.get();
    target = std::move(moved);
    require(moved.get() == -1 && moved.port() == 0 && target.get() == original_fd
                && ::fcntl(retired, F_GETFD) == -1 && errno == EBADF,
        "move assignment closes old listener");
    auto *alias = &target;
    target = std::move(*alias);
    require(target.get() == original_fd && target.port() == port, "self move preserves");
    UniqueFd output;
    for (const auto result : {target.accept_into(nullptr, 1, 1),
             target.accept_into(&output, 0, 1),
             target.accept_into(&output, 1, 0)})
        require(result.attempts == 0 && result.accepted == 0 && result.stop == AcceptStop::error
                    && error_is(result.error, EINVAL),
            "invalid batch precheck");
    UniqueFd occupied(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    const int protected_fd = occupied.get();
    auto refused = target.accept_into(&occupied, 1, 2);
    require(refused.attempts == 0 && error_is(refused.error, EINVAL)
                && occupied.get() == protected_fd && ::fcntl(protected_fd, F_GETFD) >= 0,
        "occupied output preserved");
    // 异常展开以真实fcntl证明监听资源没有逃逸；没有向业务接口引入异常。
    int unwound = -1;
    try {
        auto guard = listener();
        unwound = guard.get();
        throw std::runtime_error("fixture unwind");
    } catch (const std::runtime_error &) {
        require(::fcntl(unwound, F_GETFD) == -1 && errno == EBADF, "listener unwind releases");
    }
}
void fd_zero(bool accepted_zero) {
    const pid_t child = ::fork();
    require(child >= 0, "isolated fd0 child");
    if (child == 0) {
        if (!accepted_zero) {
            (void)::close(STDIN_FILENO);
            auto acceptor = listener();
            require(acceptor.get() == 0 && acceptor.port() != 0, "listener fd0 valid");
            require(!acceptor.close(), "close owned listener zero");
        } else {
            auto acceptor = listener();
            auto client = connect_to(IpFamily::v4, acceptor.port());
            (void)::close(STDIN_FILENO);
            auto result = acceptor.accept_one();
            require(std::holds_alternative<UniqueFd>(result), "fd0 accept succeeds");
            auto accepted = std::move(std::get<UniqueFd>(result));
            require(accepted.get() == 0 && static_cast<bool>(accepted), "accepted fd0 owned");
            flags(accepted.get());
            require(!accepted.close(), "close owned accepted zero");
        }
        std::exit(0);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fd0 child completes");
}
} // namespace
int main() {
    const auto before = fd_count();
    basic(IpFamily::v4);
    basic(IpFamily::v6);
    budget_and_lt();
    validation_and_ownership();
    fd_zero(false);
    fd_zero(true);
    require(fd_count() == before, "all test-owned fds released");
    std::cout << "parent_checks=" << checks << " isolated_children=2 failures=0\n";
}
