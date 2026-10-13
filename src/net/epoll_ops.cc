/*
 * PROJECT : SIFTWING
 * FILE    : epoll_ops.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 单次LT epoll控制，不隐藏重试、状态镜像或目标fd清理
 */
#include "siftwing/net/epoll_ops.hpp"
#include <cerrno>

namespace siftwing::net {
namespace {
std::error_code control(
    int epoll_fd, int operation, int fd, std::uint32_t interests, std::uint64_t token) noexcept {
    if (epoll_fd < 0 || fd < 0)
        return {EBADF, std::generic_category()};
    if ((interests & ~epoll_interest_mask) != 0)
        return {EINVAL, std::generic_category()};
    epoll_event event{};
    event.events = interests;
    event.data.u64 = token;
    // DEL不使用事件参数；ADD/MOD复制完整掩码和u64，避免union其他成员覆盖标识。
    const int result =
        ::epoll_ctl(epoll_fd, operation, fd, operation == EPOLL_CTL_DEL ? nullptr : &event);
    return result < 0 ? std::error_code(errno, std::generic_category()) : std::error_code{};
}
} // namespace
EpollResult create_epoll() noexcept {
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0)
        return std::error_code(errno, std::generic_category());
    return UniqueFd(fd);
}
std::error_code epoll_add(
    int epoll_fd, int fd, std::uint32_t interests, std::uint64_t token) noexcept {
    return control(epoll_fd, EPOLL_CTL_ADD, fd, interests, token);
}
std::error_code epoll_modify(
    int epoll_fd, int fd, std::uint32_t interests, std::uint64_t token) noexcept {
    return control(epoll_fd, EPOLL_CTL_MOD, fd, interests, token);
}
std::error_code epoll_remove(int epoll_fd, int fd) noexcept {
    return control(epoll_fd, EPOLL_CTL_DEL, fd, 0, 0);
}
} // namespace siftwing::net
