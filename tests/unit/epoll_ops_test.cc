/*
 * PROJECT : SIFTWING
 * FILE    : epoll_ops_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 真实内核验证LT掩码、u64标识、失败保持和注册资源边界
 */
#include "siftwing/net/epoll_ops.hpp"
#include "siftwing/net/socket_ops.hpp"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/socket.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "epoll check failed: " << label << " errno=" << errno << '\n';
        std::exit(1);
    }
}
bool error_is(std::error_code error, int value) {
    return error.value() == value && error.category() == std::generic_category();
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
UniqueFd poller() {
    auto result = create_epoll();
    require(std::holds_alternative<UniqueFd>(result), "create epoll succeeds");
    return std::move(std::get<UniqueFd>(result));
}
std::array<UniqueFd, 2> pair() {
    int fds[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0,
        "nonblocking socketpair fixture");
    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}
epoll_event ready(int epoll_fd, std::uint64_t token, std::uint32_t mask) {
    epoll_event event{};
    require(::epoll_wait(epoll_fd, &event, 1, 1000) == 1, "bounded raw fixture wait");
    require(event.data.u64 == token && (event.events & mask) == mask, "kernel token and events");
    return event;
}
void quiet(int epoll_fd) {
    epoll_event event{};
    require(::epoll_wait(epoll_fd, &event, 1, 0) == 0, "no pending event");
}
void send_byte(int fd) {
    auto result = send_some(fd, "x");
    require(result.state == IoState::progress && result.bytes == 1, "fixture byte sent");
}
void read_byte(int fd) {
    char byte = 0;
    auto result = receive_some(fd, &byte, 1);
    require(result.state == IoState::progress && result.bytes == 1 && byte == 'x',
        "fixture byte consumed");
}
void registration() {
    auto epoll = poller();
    require((::fcntl(epoll.get(), F_GETFD) & FD_CLOEXEC) != 0, "atomic epoll CLOEXEC");
    auto sockets = pair();
    constexpr std::uint64_t first = 0xfedcba9876543210ULL;
    constexpr auto second = std::numeric_limits<std::uint64_t>::max();
    require(!epoll_add(epoll.get(), sockets[0].get(), EPOLLIN, first), "add IN and full token");
    quiet(epoll.get());
    send_byte(sockets[1].get());
    const auto cached = ready(epoll.get(), first, EPOLLIN);
    ready(epoll.get(), first, EPOLLIN); // 未消费数据仍通知，独立内核观察证明LT重复就绪。
    require(!epoll_modify(epoll.get(), sockets[0].get(), EPOLLIN, second), "replace full token");
    ready(epoll.get(), second, EPOLLIN);
    require(cached.data.u64 == first, "already fetched event keeps old token");
    require(error_is(epoll_add(epoll.get(), sockets[0].get(), EPOLLOUT, 0), EEXIST),
        "duplicate ADD explicitly fails");
    ready(epoll.get(), second, EPOLLIN);
    require(error_is(epoll_modify(epoll.get(), sockets[0].get(), EPOLLET, 0), EINVAL),
        "reject ET without changing registered state");
    ready(epoll.get(), second, EPOLLIN);
    require(!epoll_modify(epoll.get(), sockets[0].get(), EPOLLOUT, 0), "replace IN with OUT");
    const auto writable = ready(epoll.get(), 0, EPOLLOUT);
    require((writable.events & EPOLLIN) == 0, "MOD replaces rather than merges mask");
    require(!epoll_modify(epoll.get(), sockets[0].get(), 0, second), "zero mask pauses IO");
    quiet(epoll.get());
    read_byte(sockets[0].get());
    require(!epoll_modify(epoll.get(), sockets[0].get(), EPOLLIN, first), "resume IN");
    quiet(epoll.get());
    send_byte(sockets[1].get());
    const auto before_del = ready(epoll.get(), first, EPOLLIN);
    require(!epoll_remove(epoll.get(), sockets[0].get()), "DEL succeeds");
    quiet(epoll.get());
    require(before_del.data.u64 == first, "DEL does not rewrite cached events");
    read_byte(sockets[0].get());
    require(::fcntl(sockets[0].get(), F_GETFD) >= 0, "DEL does not close target");
    require(error_is(epoll_remove(epoll.get(), sockets[0].get()), ENOENT), "repeat DEL fails");
    require(error_is(epoll_modify(epoll.get(), sockets[0].get(), EPOLLIN, 9), ENOENT),
        "missing MOD does not silently ADD");
    // 穷举三关注位的8种组合；每次以真实内核就绪位而非内部镜像作为参考。
    constexpr std::array<std::uint32_t, 8> masks{0,
        EPOLLIN,
        EPOLLOUT,
        EPOLLRDHUP,
        EPOLLIN | EPOLLOUT,
        EPOLLIN | EPOLLRDHUP,
        EPOLLOUT | EPOLLRDHUP,
        epoll_interest_mask};
    for (const auto mask : masks) {
        require(!epoll_add(epoll.get(), sockets[0].get(), mask, mask), "all LT subsets allowed");
        if ((mask & EPOLLOUT) != 0)
            ready(epoll.get(), mask, EPOLLOUT);
        else
            quiet(epoll.get());
        require(!epoll_remove(epoll.get(), sockets[0].get()), "subset registration removed");
    }
}
void closure() {
    auto epoll = poller();
    auto sockets = pair();
    require(!epoll_add(epoll.get(), sockets[0].get(), EPOLLIN | EPOLLRDHUP, 12), "halfclose IN");
    send_byte(sockets[1].get());
    require(::shutdown(sockets[1].get(), SHUT_WR) == 0, "peer halfclose fixture");
    ready(epoll.get(), 12, EPOLLIN | EPOLLRDHUP);
    read_byte(sockets[0].get());
    char byte = 0;
    require(receive_some(sockets[0].get(), &byte, 1).state == IoState::eof, "pending then EOF");
    require(!epoll_modify(epoll.get(), sockets[0].get(), 0, 15), "zero mask after halfclose");
    quiet(epoll.get()); // RDHUP需要订阅，半关闭此时没有完整HUP。
    require(!sockets[1].close(), "peer full close");
    ready(epoll.get(), 15, EPOLLHUP); // HUP即使关注位为0也由内核自动交付。
    require(!epoll_remove(epoll.get(), sockets[0].get()), "remove HUP target");
}
void failures_and_ownership() {
    auto sockets = pair();
    int retired = -1;
    {
        auto epoll = poller();
        retired = epoll.get();
        require(error_is(epoll_add(-1, sockets[0].get(), EPOLLIN, 1), EBADF), "bad epoll ADD");
        require(error_is(epoll_modify(epoll.get(), -1, EPOLLIN, 1), EBADF), "bad target MOD");
        require(error_is(epoll_remove(-1, -1), EBADF), "bad DEL");
        require(error_is(epoll_add(epoll.get(), epoll.get(), EPOLLIN, 1), EINVAL), "self ADD");
        UniqueFd file(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        require(static_cast<bool>(file), "regular file fixture");
        require(error_is(epoll_add(epoll.get(), file.get(), EPOLLIN, 1), EPERM), "unsupported fd");
        require(::fcntl(file.get(), F_GETFD) >= 0, "failed ADD preserves target");
        require(error_is(epoll_add(sockets[1].get(), sockets[0].get(), EPOLLIN, 1), EINVAL),
            "non-epoll descriptor");
        require(!epoll_add(epoll.get(), sockets[0].get(), EPOLLIN, 1), "valid ADD after failures");
    }
    require(
        ::fcntl(retired, F_GETFD) == -1 && errno == EBADF, "epoll destructor closes only epoll");
    send_byte(sockets[1].get());
    read_byte(sockets[0].get());
    auto epoll = poller();
    require(!epoll_add(epoll.get(), sockets[0].get(), EPOLLIN, 2), "add before close");
    require(!sockets[0].close(), "close last target descriptor");
    quiet(epoll.get()); // 没有别名时最后一个close移除内核注册。
    require(error_is(epoll_remove(epoll.get(), -1), EBADF), "closed owner cannot DEL");
}
void alias_lifetime() {
    // Linux注册键含fd数字与open file description；dup别名使原注册在close原数字后存活。
    auto epoll = poller();
    auto sockets = pair();
    UniqueFd alias(::fcntl(sockets[0].get(), F_DUPFD_CLOEXEC, 0));
    require(static_cast<bool>(alias), "dup fixture");
    require(!epoll_add(epoll.get(), sockets[0].get(), EPOLLIN, 0x1234), "register original fd");
    require(!sockets[0].close(), "close original while alias lives");
    send_byte(sockets[1].get());
    ready(epoll.get(), 0x1234, EPOLLIN);
    require(error_is(epoll_remove(epoll.get(), alias.get()), ENOENT), "alias is a different key");
    require(!alias.close(), "close last alias removes old registration");
    quiet(epoll.get());
}
void fd_zero() {
    const auto child = ::fork();
    require(child >= 0, "fork isolated fd0 child");
    if (child == 0) {
        (void)::close(STDIN_FILENO);
        auto result = create_epoll();
        require(std::holds_alternative<UniqueFd>(result), "child creates epoll");
        auto &epoll = std::get<UniqueFd>(result);
        require(epoll.get() == 0 && static_cast<bool>(epoll), "epoll fd0 is owned");
        require(!epoll.close() && ::fcntl(0, F_GETFD) == -1 && errno == EBADF, "child closes fd0");
        _exit(0);
    }
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "fd0 child succeeds");
}
} // namespace
int main() {
    static_assert(std::is_nothrow_move_constructible_v<EpollResult>);
    static_assert(noexcept(create_epoll()) && noexcept(epoll_add(0, 1, 0, 0))
                  && noexcept(epoll_modify(0, 1, 0, 0)) && noexcept(epoll_remove(0, 1)));
    const auto before = fd_count();
    registration();
    closure();
    failures_and_ownership();
    alias_lifetime();
    fd_zero();
    require(fd_count() == before, "all fixture/epoll descriptors released");
    std::cout << "parent_checks=" << checks << " isolated_children=1 failures=0\n";
}
