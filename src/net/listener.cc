/*
 * PROJECT : SIFTWING
 * FILE    : listener.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 监听资源事务式创建及按尝试次数计数的Linux接入
 */
#include "siftwing/net/listener.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <utility>

namespace siftwing::net {
namespace {
std::error_code failure(int value) noexcept {
    return {value, std::generic_category()};
}
bool retryable(int value) noexcept {
    // Linux accept文档允许将这些pending network errors按EAGAIN类再尝试；预算避免错误风暴占满线程。
    switch (value) {
        case EINTR:
        case ECONNABORTED:
        case ENETDOWN:
        case EPROTO:
        case ENOPROTOOPT:
        case EHOSTDOWN:
        case ENONET:
        case EHOSTUNREACH:
        case EOPNOTSUPP:
        case ENETUNREACH:
            return true;
        default:
            return false;
    }
}
} // namespace
ListenerResult Listener::create(const ListenConfig &config) noexcept {
    if ((config.family != IpFamily::v4 && config.family != IpFamily::v6) || config.backlog <= 0
        || config.address.empty() || config.address.size() >= INET6_ADDRSTRLEN
        || config.address.find('\0') != std::string_view::npos)
        return failure(EINVAL);
    // inet_pton需要NUL结尾；有界栈复制不保存配置借用，也不引入分配异常。
    std::array<char, INET6_ADDRSTRLEN> text{};
    std::memcpy(text.data(), config.address.data(), config.address.size());
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    const bool ipv6 = config.family == IpFamily::v6;
    v4.sin_family = AF_INET;
    v4.sin_port = htons(config.port);
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(config.port);
    const int parsed = ipv6 ? ::inet_pton(AF_INET6, text.data(), &v6.sin6_addr)
                            : ::inet_pton(AF_INET, text.data(), &v4.sin_addr);
    if (parsed != 1)
        return failure(parsed == 0 ? EINVAL : errno);
    auto opened = open_tcp_socket(config.family);
    if (auto *error = std::get_if<std::error_code>(&opened))
        return *error;
    UniqueFd socket(std::move(std::get<UniqueFd>(opened)));
    const int enabled = 1;
    if (ipv6
        && ::setsockopt(socket.get(), IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled)) < 0)
        return failure(errno);
    if (config.reuse_address
        && ::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0)
        return failure(errno);
    const auto *address =
        ipv6 ? reinterpret_cast<const sockaddr *>(&v6) : reinterpret_cast<const sockaddr *>(&v4);
    const auto length = static_cast<socklen_t>(ipv6 ? sizeof(v6) : sizeof(v4));
    if (::bind(socket.get(), address, length) < 0 || ::listen(socket.get(), config.backlog) < 0)
        return failure(errno);
    sockaddr_storage local{};
    socklen_t local_size = sizeof(local);
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr *>(&local), &local_size) < 0)
        return failure(errno);
    if (local.ss_family != (ipv6 ? AF_INET6 : AF_INET) || local_size < length)
        return failure(EIO);
    // 从通用地址存储复制回匹配类型，避免通过不匹配的C++对象类型读取内存。
    if (ipv6)
        std::memcpy(&v6, &local, sizeof(v6));
    else
        std::memcpy(&v4, &local, sizeof(v4));
    Listener result;
    result.port_ = ntohs(ipv6 ? v6.sin6_port : v4.sin_port);
    result.socket_ = std::move(socket);
    return result;
}
SocketResult Listener::accept_one() const noexcept {
    if (socket_.get() < 0)
        return failure(EBADF);
    const int fd = ::accept4(socket_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return failure(errno);
    return UniqueFd(fd);
}
AcceptBatch Listener::accept_into(
    UniqueFd *destination, std::size_t capacity, std::size_t attempt_budget) const noexcept {
    if (socket_.get() < 0)
        return {0, 0, AcceptStop::error, failure(EBADF)};
    if (destination == nullptr || capacity == 0 || attempt_budget == 0)
        return {0, 0, AcceptStop::error, failure(EINVAL)};
    for (std::size_t i = 0; i < std::min(capacity, attempt_budget); ++i)
        if (destination[i])
            return {0, 0, AcceptStop::error, failure(EINVAL)};
    AcceptBatch batch;
    while (batch.attempts < attempt_budget) {
        ++batch.attempts;
        auto result = accept_one();
        if (auto *accepted = std::get_if<UniqueFd>(&result)) {
            destination[batch.accepted++] = std::move(*accepted);
            if (batch.accepted == capacity) {
                batch.stop = AcceptStop::output_full;
                return batch;
            }
        } else {
            const auto error = std::get<std::error_code>(result);
            if (error.value() == EAGAIN || error.value() == EWOULDBLOCK)
                return {batch.attempts, batch.accepted, AcceptStop::drained, error};
            if (!retryable(error.value()))
                return {batch.attempts, batch.accepted, AcceptStop::error, error};
        }
    }
    batch.stop = AcceptStop::budget_exhausted;
    return batch;
}
} // namespace siftwing::net
