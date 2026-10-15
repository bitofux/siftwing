/*
 * PROJECT : SIFTWING
 * FILE    : client.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : poll等待与一次请求的固定总期限
 * IMPLEMENTATION : 非阻塞I/O先尝试再等就绪；每次重试核同一deadline，codec负责流边界。
 */
#include "siftwing/net/client.hpp"
#include "siftwing/net/unique_fd.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace siftwing::net {
namespace {
using Clock = std::chrono::steady_clock;
struct Failure {
    base::Error error;
};
[[noreturn]] void fail(const char *where, const char *message) {
    throw Failure{{message, where}};
}
[[noreturn]] void system_failure(const char *where) {
    throw Failure{{std::error_code(errno, std::generic_category()).message(), where}};
}
void check_time(Clock::time_point end) {
    if (Clock::now() >= end)
        fail("client.timeout", "请求总期限已到");
}
void wait(int fd, short events, Clock::time_point end) {
    for (;;) {
        check_time(end);
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
        pollfd item{fd, events, 0};
        const int result = ::poll(&item, 1, static_cast<int>(remaining > 0 ? remaining + 1 : 1));
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            system_failure("client.poll");
        check_time(end);
        if (!result)
            continue;
        if (item.revents & POLLNVAL)
            fail("client.poll", "无效socket");
        // HUP/ERR也交回实际I/O或SO_ERROR，不能把就绪当成业务成功。
        if (item.revents & (events | POLLHUP | POLLERR))
            return;
    }
}
MessageType expected(MessageType request) {
    switch (request) {
        case MessageType::ping:
            return MessageType::pong;
        case MessageType::server_info_request:
            return MessageType::server_info_response;
        case MessageType::search_request:
            return MessageType::search_response;
        case MessageType::suggest_request:
            return MessageType::suggest_response;
        default:
            fail("client.request", "非法请求类型");
    }
}
}
base::Result<Frame> exchange(const ClientConfig &config, const Frame &request) {
    using Result = base::Result<Frame>;
    try {
        if (!config.port || !config.timeout_ms || config.timeout_ms > 86400000
            || config.max_response_bytes < 2 || config.max_response_bytes > frame_max_body_bytes
            || config.host.empty() || config.host.find('\0') != std::string::npos)
            fail("client.config", "非法客户端配置");
        sockaddr_storage address{};
        socklen_t length = 0;
        int family = AF_INET;
        sockaddr_in ipv4{};
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(config.port);
        sockaddr_in6 ipv6{};
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(config.port);
        if (::inet_pton(AF_INET, config.host.c_str(), &ipv4.sin_addr) == 1) {
            std::memcpy(&address, &ipv4, sizeof(ipv4));
            length = sizeof(ipv4);
        } else if (::inet_pton(AF_INET6, config.host.c_str(), &ipv6.sin6_addr) == 1) {
            family = AF_INET6;
            std::memcpy(&address, &ipv6, sizeof(ipv6));
            length = sizeof(ipv6);
        } else
            fail("client.config", "host须为数字IPv4或IPv6");
        auto encoded = encode_frame(request, ReceiveRole::server);
        const auto *wire = std::get_if<std::string>(&encoded);
        if (!wire)
            fail("client.request", "请求不符合帧协议");
        const auto response_type = expected(request.message_type);
        const auto end = Clock::now() + std::chrono::milliseconds(config.timeout_ms);
        UniqueFd socket(::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!socket)
            system_failure("client.socket");
        const int fd = socket.get();
        check_time(end);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), length) < 0) {
            if (errno != EINPROGRESS && errno != EINTR)
                system_failure("client.connect");
            wait(fd, POLLOUT, end);
            int error = 0;
            socklen_t size = sizeof(error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
                system_failure("client.connect");
            if (error) {
                errno = error;
                system_failure("client.connect");
            }
        }
        std::size_t offset = 0;
        while (offset < wire->size()) {
            check_time(end);
            const auto count =
                ::send(fd, wire->data() + offset, wire->size() - offset, MSG_NOSIGNAL);
            if (count > 0)
                offset += static_cast<std::size_t>(count);
            else if (count < 0 && errno == EINTR)
                continue;
            else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                wait(fd, POLLOUT, end);
            else if (count < 0)
                system_failure("client.send");
            else
                fail("client.send", "发送无进展");
        }
        Buffer input(frame_header_bytes + config.max_response_bytes);
        std::array<char, 4096> bytes{};
        for (;;) {
            check_time(end);
            auto decoded = decode_next(input, ReceiveRole::client, config.max_response_bytes);
            if (auto *frame = std::get_if<Frame>(&decoded)) {
                if (input.readable_bytes() || frame->request_id != request.request_id
                    || (frame->message_type != response_type
                        && frame->message_type != MessageType::error_response)
                    || validate_frame(*frame, ReceiveRole::client, config.max_response_bytes))
                    fail("client.response", "响应类型、关联号、Body或帧余量非法");
                check_time(end);
                return Result::success(std::move(*frame));
            }
            if (std::holds_alternative<DecodeError>(decoded))
                fail("client.response", "响应头非法或超预算");
            const auto room = input.max_bytes() - input.readable_bytes();
            if (!room)
                fail("client.response", "响应超预算");
            check_time(end);
            const auto count = ::recv(fd, bytes.data(), std::min(room, bytes.size()), 0);
            if (count > 0) {
                if (!input.append({bytes.data(), static_cast<std::size_t>(count)}))
                    fail("client.response", "响应Buffer超限");
            } else if (count == 0)
                fail("client.eof", "响应未完整时对端关闭");
            else if (errno == EINTR)
                continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
                wait(fd, POLLIN, end);
            else
                system_failure("client.recv");
        }
    } catch (const Failure &failure) {
        return Result::failure(failure.error);
    }
}
} // namespace siftwing::net
