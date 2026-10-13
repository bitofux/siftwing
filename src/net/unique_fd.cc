/*
 * PROJECT : SIFTWING
 * FILE    : unique_fd.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 转交fd所有权并实现Linux不重试close的资源释放
 */
#include "siftwing/net/unique_fd.hpp"
#include <cerrno>
#include <unistd.h>
#include <utility>

namespace siftwing::net {
UniqueFd::UniqueFd(int fd) noexcept : fd_(fd >= 0 ? fd : -1) {}
UniqueFd::~UniqueFd() noexcept {
    (void)close();
}
UniqueFd::UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}
UniqueFd &UniqueFd::operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
        (void)close();
        fd_ = other.release();
    }
    return *this;
}
int UniqueFd::release() noexcept {
    return std::exchange(fd_, -1);
}
std::error_code UniqueFd::close() noexcept {
    const int retired = release();
    if (retired < 0)
        return {};
    // 关闭前清空状态；Linux先释放fd槽，重试EINTR可能误关其他线程新打开的资源。
    const int saved = errno;
    const int result = ::close(retired);
    const int failure = errno;
    errno = saved;
    return result < 0 ? std::error_code(failure, std::generic_category()) : std::error_code{};
}
} // namespace siftwing::net
