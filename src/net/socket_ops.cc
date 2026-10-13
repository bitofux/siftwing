/*
 * PROJECT : SIFTWING
 * FILE    : socket_ops.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : Linux原子socket标志及显式单次收发分支，不包含连接调度
 */
#include "siftwing/net/socket_ops.hpp"
#include <cerrno>
#include <limits>
#include <netinet/in.h>
#include <sys/socket.h>

namespace siftwing::net {
namespace {
IoResult failure(int reason) noexcept {
    IoState state = IoState::error;
    if (reason == EAGAIN || reason == EWOULDBLOCK)
        state = IoState::would_block;
    else if (reason == EINTR)
        state = IoState::interrupted;
    return {state, 0, {reason, std::generic_category()}};
}
constexpr auto max_io = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
} // namespace
SocketResult open_tcp_socket(IpFamily family) noexcept {
    int domain = 0;
    switch (family) {
        case IpFamily::v4:
            domain = AF_INET;
            break;
        case IpFamily::v6:
            domain = AF_INET6;
            break;
        default:
            return std::error_code(EINVAL, std::generic_category());
    }
    // 创建时一起设置，避免socket→fcntl之间被另一个线程exec继承或观察阻塞fd。
    const int fd = ::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0)
        return std::error_code(errno, std::generic_category());
    return UniqueFd(fd);
}
IoResult receive_some(int fd, char *destination, std::size_t capacity) noexcept {
    if (fd < 0)
        return failure(EBADF);
    if (capacity > max_io || (capacity != 0 && destination == nullptr))
        return failure(EINVAL);
    if (capacity == 0)
        return {IoState::progress, 0, {}};
    const auto count = ::recv(fd, destination, capacity, MSG_DONTWAIT);
    if (count < 0)
        return failure(errno);
    if (count == 0)
        return {IoState::eof, 0, {}};
    return {IoState::progress, static_cast<std::size_t>(count), {}};
}
IoResult send_some(int fd, std::string_view bytes) noexcept {
    if (fd < 0)
        return failure(EBADF);
    if (bytes.size() > max_io)
        return failure(EINVAL);
    if (bytes.empty())
        return {IoState::progress, 0, {}};
    const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count < 0)
        return failure(errno);
    return {IoState::progress, static_cast<std::size_t>(count), {}};
}
} // namespace siftwing::net
