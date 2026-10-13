/*
 * PROJECT : SIFTWING
 * FILE    : connection_write_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 独立线字节、真实短写、epoll掩码和TCP验证有界响应输出
 */
#include "siftwing/net/connection.hpp"
#include "siftwing/net/listener.hpp"
#include "siftwing/net/socket_ops.hpp"
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        std::cerr << "connection write failed: " << message << '\n';
        std::exit(1);
    }
}
struct Pair {
    UniqueFd server;
    UniqueFd peer;
};
Pair pair(bool nonblocking = true) {
    int fd[2];
    require(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0), 0, fd)
            == 0,
        "real stream pair");
    return {UniqueFd(fd[0]), UniqueFd(fd[1])};
}
// 期望头按偏移独立写出，不调用生产encoder/decoder，避免同源缺陷自证。
std::string wire(std::uint16_t type,
    std::string_view body = "{}",
    std::uint64_t id = 0x0102030405060708ULL,
    std::uint16_t status = 0) {
    std::string bytes(24, '\0');
    bytes.replace(0, 4, "SFTW");
    bytes[4] = 1;
    bytes[5] = 24;
    const auto put = [&](std::size_t offset, unsigned int width, std::uint64_t value) {
        for (unsigned int i = 0; i < width; ++i)
            bytes[offset + i] = static_cast<char>((value >> ((width - i - 1) * 8)) & 255U);
    };
    put(6, 2, type);
    put(10, 2, status);
    put(12, 4, body.size());
    put(16, 8, id);
    bytes.append(body);
    return bytes;
}
void sent(int fd, std::string_view bytes) {
    require(
        ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size()),
        "small request fixture sent");
}
ReadResult request(Connection &connection,
    int peer,
    std::uint16_t type = 3,
    std::string_view body = "{}",
    std::uint64_t id = 0x0102030405060708ULL) {
    sent(peer, wire(type, body, id));
    pollfd event{connection.get(), POLLIN, 0};
    require(::poll(&event, 1, 2000) == 1, "bounded request readiness");
    auto result = connection.read_request();
    require(result.stop == ReadStop::request && result.request, "request context acquired");
    return result;
}
void collect(int fd, std::string &bytes) {
    std::array<char, 8192> block{};
    for (unsigned int i = 0; i < 1024; ++i) {
        const auto n = ::recv(fd, block.data(), block.size(), MSG_DONTWAIT);
        if (n > 0) {
            bytes.append(block.data(), static_cast<std::size_t>(n));
            continue;
        }
        require(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR),
            "peer drain stops at temporary boundary");
        if (errno != EINTR)
            return;
    }
    require(false, "bounded peer drain");
}
void mapping_and_gate() {
    const std::array<MessageType, 4> requests{{MessageType::ping,
        MessageType::server_info_request,
        MessageType::search_request,
        MessageType::suggest_request}};
    const std::array<MessageType, 4> responses{{MessageType::pong,
        MessageType::server_info_response,
        MessageType::search_response,
        MessageType::suggest_response}};
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto sockets = pair();
        Connection connection(std::move(sockets.server), 256);
        Frame response{responses[i], 0, UINT64_MAX, "{}"};
        require(!connection.needs_write() && connection.output_bytes() == 0
                    && connection.flush_output(0, 0).stop == WriteStop::drained,
            "empty output before zero budget, no write intent");
        require(connection.queue_response(response).stop == QueueStop::no_request,
            "no unsolicited response");
        auto read = request(connection,
            sockets.peer.get(),
            static_cast<std::uint16_t>(requests[i]),
            "{}",
            UINT64_MAX);
        require(!read.body_error && connection.queue_response(response).stop == QueueStop::queued,
            "four explicit success type mappings and maximum id");
        const auto expected = wire(static_cast<std::uint16_t>(responses[i]), "{}", UINT64_MAX);
        response.body = "{\"changed\":1}";
        response.request_id = 7;
        require(connection.output_bytes() == expected.size() && connection.needs_write()
                    && !connection.finish_request(),
            "owned wire independent of caller and early finish refused");
        require(connection.queue_response(response).stop == QueueStop::response_pending,
            "duplicate refused before mismatch validation");
        const auto invalid = connection.flush_output(0, 1);
        require(invalid.stop == WriteStop::invalid_budget && invalid.error.value() == EINVAL
                    && invalid.attempts == 0 && connection.output_bytes() == expected.size(),
            "zero budget preserves full output");
        require(connection.flush_output(1, 0).stop == WriteStop::invalid_budget,
            "zero attempt budget refused");
        require(connection.read_request(0, 0).stop == ReadStop::request_pending,
            "queued response retains read gate");
        std::string actual;
        // 穷举每个线字节的发送边界；最后一字节排空必须优先于两个预算到达。
        for (std::size_t n = 0; n < expected.size(); ++n) {
            const auto written = connection.flush_output(1, 1);
            require(written.attempts == 1 && written.bytes == 1 && !written.error
                        && written.stop
                               == (n + 1 == expected.size() ? WriteStop::drained
                                                            : WriteStop::budget_exhausted),
                "one byte exact progress");
            require(connection.output_bytes() == expected.size() - n - 1
                        && connection.needs_write() == (n + 1 != expected.size()),
                "suffix and write intent");
            collect(sockets.peer.get(), actual);
        }
        require(actual == expected && connection.request_pending(),
            "exact independent wire, gate not automatic");
        require(connection.queue_response(response).stop == QueueStop::response_pending,
            "drained response cannot be enqueued again");
        require(
            connection.finish_request() && !connection.finish_request(), "finish once after drain");
    }
    auto sockets = pair();
    Connection connection(std::move(sockets.server), 128);
    sent(sockets.peer.get(), wire(1, "{}", 1) + wire(1, "{}", 2));
    require(connection.read_request().request->request_id == 1, "first coalesced request");
    require(connection.queue_response({MessageType::pong, 0, 1, "{}"}).stop == QueueStop::queued
                && connection.flush_output().stop == WriteStop::drained
                && connection.finish_request(),
        "first response fully written");
    const auto next = connection.read_request(1, 1);
    require(next.request && next.request->request_id == 2 && next.attempts == 0,
        "caller actively schedules cached next frame");
}
void refusals_and_errors() {
    auto sockets = pair();
    Connection connection(std::move(sockets.server), 256);
    (void)request(connection, sockets.peer.get());
    require(connection.queue_response({MessageType::search_response, 0, 8, "{}"}).stop
                == QueueStop::request_mismatch,
        "wrong id does not consume gate");
    require(connection.queue_response({MessageType::pong, 0, 0x0102030405060708ULL, "{}"}).stop
                == QueueStop::request_mismatch,
        "wrong success type refused");
    for (const auto error : {ProtocolError::json,
             ProtocolError::json_object,
             ProtocolError::status,
             ProtocolError::body_length,
             ProtocolError::utf8}) {
        Frame response{MessageType::search_response, 0, 0x0102030405060708ULL, "{}"};
        if (error == ProtocolError::json)
            response.body = "{]";
        if (error == ProtocolError::json_object)
            response.body = "[]";
        if (error == ProtocolError::status)
            response.status = 1;
        if (error == ProtocolError::body_length)
            response.body.assign(257, ' ');
        if (error == ProtocolError::utf8)
            response.body = std::string("{\"x\":\"\xff\"}", 9);
        const auto result = connection.queue_response(response);
        require(result.stop == QueueStop::protocol_error && result.protocol_error == error
                    && connection.request_pending() && !connection.needs_write()
                    && connection.output_bytes() == 0,
            "codec failure leaves context and output unchanged");
    }
    for (std::uint16_t status = 1; status <= 6; ++status) {
        auto bad = pair();
        Connection rejected(std::move(bad.server), 128);
        const auto read = request(rejected, bad.peer.get(), 3, "{]", status);
        require(read.body_error.has_value(), "trusted header but rejected Body");
        require(rejected.queue_response({MessageType::search_response, 0, status, "{}"}).stop
                    == QueueStop::request_mismatch,
            "Body rejection forbids success response");
        const Frame error{MessageType::error_response, status, status, "{\"message\":\"test\"}"};
        require(rejected.queue_response(error).stop == QueueStop::queued
                    && rejected.flush_output().stop == WriteStop::drained,
            "six known error statuses sent");
        std::string actual;
        collect(bad.peer.get(), actual);
        require(actual == wire(0xffff, error.body, status, status),
            "error preserves trusted id and status");
    }
    auto tiny = pair();
    Connection minimum(std::move(tiny.server), 2);
    (void)request(minimum, tiny.peer.get(), 1);
    require(minimum.queue_response({MessageType::pong, 0, 0x0102030405060708ULL, "{}"}).stop
                    == QueueStop::queued
                && minimum.output_bytes() == 26,
        "minimum valid output limit");
    require(!minimum.close() && !minimum.needs_write() && minimum.output_bytes() == 0
                && minimum.queue_response({MessageType::pong, 0, 1, "{}"}).stop == QueueStop::closed
                && minimum.flush_output(0, 0).stop == WriteStop::closed,
        "explicit close drops output once");
}
void large_and_epoll() {
    // 刻意使用阻塞fd；生产send的MSG_DONTWAIT仍须在小内核缓冲下返回EAGAIN，不能挂住。
    auto sockets = pair(false);
    Connection connection(std::move(sockets.server));
    const int small = 4096;
    require(::setsockopt(connection.get(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0,
        "small kernel send buffer");
    (void)request(connection, sockets.peer.get());
    UniqueFd epoll(::epoll_create1(EPOLL_CLOEXEC));
    require(epoll.get() >= 0, "real epoll");
    const auto interest = [&](int operation) {
        epoll_event event{};
        event.events = EPOLLIN | (connection.needs_write() ? EPOLLOUT : 0U);
        event.data.u64 = 91;
        require(::epoll_ctl(epoll.get(), operation, connection.get(), &event) == 0,
            "caller applies write intent to real mask");
    };
    epoll_event event{};
    interest(EPOLL_CTL_ADD);
    require(
        ::epoll_wait(epoll.get(), &event, 1, 0) == 0, "idle connection has no perpetual EPOLLOUT");
    Frame response{MessageType::search_response,
        0,
        0x0102030405060708ULL,
        "{\"x\":\"" + std::string(frame_max_body_bytes - 8, 'x') + "\"}"};
    require(response.body.size() == frame_max_body_bytes, "maximum legal Body fixture");
    const auto expected = wire(0x8003, response.body);
    require(
        connection.queue_response(response).stop == QueueStop::queued, "maximum frame accepted");
    interest(EPOLL_CTL_MOD);
    require(::epoll_wait(epoll.get(), &event, 1, 0) == 1 && (event.events & EPOLLOUT)
                && event.data.u64 == 91,
        "nonempty output enables actual write event");
    const auto first = connection.flush_output(expected.size(), 64);
    require(first.stop == WriteStop::would_block && first.bytes > 0 && first.bytes < expected.size()
                && first.error.value() == EAGAIN && first.attempts >= 2
                && connection.output_bytes() == expected.size() - first.bytes
                && connection.needs_write(),
        "real partial send then EAGAIN retains exact suffix");
    std::string actual;
    std::size_t total = first.bytes;
    for (unsigned int i = 0; connection.needs_write() && i < 1024; ++i) {
        collect(sockets.peer.get(), actual);
        const auto result = connection.flush_output();
        total += result.bytes;
        require(result.bytes <= 65536 && result.attempts <= 16
                    && (result.stop == WriteStop::drained || result.stop == WriteStop::would_block
                        || result.stop == WriteStop::budget_exhausted),
            "bounded large suffix progress");
    }
    collect(sockets.peer.get(), actual);
    require(!connection.needs_write() && total == expected.size() && actual == expected,
        "maximum frame delivered exactly once without loss or duplication");
    interest(EPOLL_CTL_MOD);
    require(::epoll_wait(epoll.get(), &event, 1, 0) == 0 && connection.finish_request(),
        "drained output disables EPOLLOUT and caller releases gate");
}
void tcp() {
    for (const auto family : {IpFamily::v4, IpFamily::v6}) {
        auto created =
            Listener::create({family, family == IpFamily::v4 ? "127.0.0.1" : "::1", 0, 8, true});
        require(std::holds_alternative<Listener>(created), "TCP listener created");
        auto &listener = std::get<Listener>(created);
        auto opened = open_tcp_socket(family);
        require(std::holds_alternative<UniqueFd>(opened), "TCP client");
        auto &client = std::get<UniqueFd>(opened);
        sockaddr_in v4{};
        v4.sin_family = AF_INET;
        v4.sin_port = htons(listener.port());
        v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sockaddr_in6 v6{};
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(listener.port());
        v6.sin6_addr = in6addr_loopback;
        const auto *address = family == IpFamily::v4 ? reinterpret_cast<const sockaddr *>(&v4)
                                                     : reinterpret_cast<const sockaddr *>(&v6);
        const auto size = static_cast<socklen_t>(family == IpFamily::v4 ? sizeof(v4) : sizeof(v6));
        const auto connected = ::connect(client.get(), address, size);
        require(
            connected == 0 || (connected < 0 && errno == EINPROGRESS), "bounded loopback connect");
        pollfd readiness{listener.get(), POLLIN, 0};
        require(::poll(&readiness, 1, 2000) == 1, "accept readiness");
        auto accepted = listener.accept_one();
        require(std::holds_alternative<UniqueFd>(accepted), "accepted TCP");
        Connection connection(std::move(std::get<UniqueFd>(accepted)));
        (void)request(connection, client.get(), 1);
        require(::shutdown(client.get(), SHUT_WR) == 0, "peer half-closes request direction");
        require(connection.queue_response({MessageType::pong, 0, 0x0102030405060708ULL, "{}"}).stop
                        == QueueStop::queued
                    && connection.flush_output().stop == WriteStop::drained,
            "response still sent after peer half-close");
        readiness = {client.get(), POLLIN, 0};
        require(::poll(&readiness, 1, 2000) == 1, "response readiness");
        std::string actual;
        collect(client.get(), actual);
        require(actual == wire(0x8001) && connection.finish_request()
                    && connection.read_request().stop == ReadStop::peer_eof,
            "real TCP response precedes observed read EOF");
    }
}
void fd_zero_and_sigpipe() {
    const auto child = ::fork();
    require(child >= 0, "isolated child");
    if (child == 0) {
        require(::signal(SIGPIPE, SIG_DFL) != SIG_ERR, "default SIGPIPE only in child");
        (void)::close(STDIN_FILENO);
        auto sockets = pair();
        require(sockets.server.get() == 0, "fd0 socket owned");
        Connection connection(std::move(sockets.server), 128);
        (void)request(connection, sockets.peer.get(), 1);
        require(connection.queue_response({MessageType::pong, 0, 0x0102030405060708ULL, "{}"}).stop
                        == QueueStop::queued
                    && connection.needs_write(),
            "fd0 has valid write intent");
        require(!sockets.peer.close(), "close peer before send");
        const auto result = connection.flush_output();
        require(result.stop == WriteStop::io_error && result.error.value() == EPIPE
                    && connection.get() == -1 && !connection.needs_write()
                    && connection.output_bytes() == 0 && !connection.request_pending(),
            "NOSIGNAL prevents process death");
        require(::fcntl(0, F_GETFD) == -1 && errno == EBADF, "fd0 actually closed once");
        ::_exit(0);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "child survived real broken peer");
}
} // namespace
int main() {
    mapping_and_gate();
    refusals_and_errors();
    large_and_epoll();
    tcp();
    fd_zero_and_sigpipe();
    std::cout << "parent_checks=" << checks << " isolated_children=1 failures=0\n";
}
