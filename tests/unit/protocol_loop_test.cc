/*
 * PROJECT : SIFTWING
 * FILE    : protocol_loop_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 独立线字节与真实IPv4/IPv6验证单Loop调度、容量和生命
 */
#include "siftwing/net/protocol_loop.hpp"
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool value, const char *message) {
    ++checks;
    if (!value) {
        std::cerr << "protocol loop failed: " << message << '\n';
        std::exit(1);
    }
}
// 独立按24字节偏移编码，不用生产codec生成期望答案。
std::string wire(
    std::uint16_t type, std::uint64_t id, std::string_view body = "{}", std::uint16_t status = 0) {
    std::string bytes(24, '\0');
    bytes.replace(0, 4, "SFTW");
    bytes[4] = 1;
    bytes[5] = 24;
    const auto put = [&](std::size_t offset, unsigned int width, std::uint64_t value) {
        for (unsigned int i = 0; i < width; ++i)
            bytes[offset + i] = static_cast<char>((value >> (8 * (width - i - 1))) & 255U);
    };
    put(6, 2, type);
    put(10, 2, status);
    put(12, 4, body.size());
    put(16, 8, id);
    bytes.append(body);
    return bytes;
}
std::unique_ptr<ProtocolLoop> loop(ProtocolLoopConfig config = {}, IpFamily family = IpFamily::v4) {
    auto created = ProtocolLoop::create(
        {family, family == IpFamily::v4 ? "127.0.0.1" : "::1", 0, 8, true}, config);
    require(std::holds_alternative<std::unique_ptr<ProtocolLoop>>(created), "real loop created");
    return std::move(std::get<std::unique_ptr<ProtocolLoop>>(created));
}
UniqueFd client(const ProtocolLoop &server, IpFamily family = IpFamily::v4) {
    auto result = open_tcp_socket(family);
    require(std::holds_alternative<UniqueFd>(result), "client socket");
    auto fd = std::move(std::get<UniqueFd>(result));
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(server.port());
    v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(server.port());
    v6.sin6_addr = in6addr_loopback;
    const auto *address = family == IpFamily::v4 ? reinterpret_cast<const sockaddr *>(&v4)
                                                 : reinterpret_cast<const sockaddr *>(&v6);
    const auto length = static_cast<socklen_t>(family == IpFamily::v4 ? sizeof(v4) : sizeof(v6));
    const int connected = ::connect(fd.get(), address, length);
    require(
        connected == 0 || (connected < 0 && errno == EINPROGRESS), "nonblocking loopback connect");
    return fd;
}
LoopStep tick(ProtocolLoop &server, int timeout = 20) {
    auto result = server.run_once(timeout);
    require(result.stop == LoopStop::progress || result.stop == LoopStop::idle,
        "finite real loop tick");
    return result;
}
void send(int fd, std::string_view bytes) {
    require(
        ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size()),
        "small fixture sent");
}
std::string receive(ProtocolLoop &server, int fd, std::size_t wanted) {
    std::string bytes;
    std::array<char, 512> block{};
    for (unsigned int i = 0; bytes.size() < wanted && i < 256; ++i) {
        (void)tick(server);
        const auto n = ::recv(fd, block.data(), block.size(), MSG_DONTWAIT);
        if (n > 0)
            bytes.append(block.data(), static_cast<std::size_t>(n));
        else
            require(
                n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK), "response still in progress");
    }
    require(bytes.size() == wanted, "finite exact response length");
    return bytes;
}
void protocol() {
    for (const auto family : {IpFamily::v4, IpFamily::v6}) {
        auto server = loop({}, family);
        auto peer = client(*server, family);
        (void)tick(*server);
        const auto one = wire(1, 1);
        send(peer.get(), one.substr(0, 9));
        auto incomplete = tick(*server);
        require(incomplete.requests == 0, "fragment cannot publish a request");
        send(peer.get(), one.substr(9) + wire(1, UINT64_MAX));
        const auto first = tick(*server);
        require(first.requests == 1 && first.completed_responses == 1 && first.read_bytes >= 17
                    && first.read_bytes <= 43,
            "one frame per turn, TCP may split the same write");
        auto next = server->run_once(20);
        for (unsigned int i = 0; next.requests == 0 && first.read_bytes != 43 && i < 8; ++i)
            next = tick(*server);
        require(next.requests == 1 && next.completed_responses == 1
                    && (first.read_bytes != 43 || next.read_attempts == 0),
            "coalesced cached request proceeds without new recv; split TCP still progresses");
        require(receive(*server, peer.get(), 52) == wire(0x8001, 1) + wire(0x8001, UINT64_MAX),
            "two independent PONG wires");
        // 半关闭只结束请求方向；输出仍应排空后再观察EOF。
        send(peer.get(), wire(1, 3));
        require(::shutdown(peer.get(), SHUT_WR) == 0, "half close");
        require(receive(*server, peer.get(), 26) == wire(0x8001, 3), "PONG after peer FIN");
        for (unsigned int i = 0; server->connection_count() && i < 8; ++i)
            (void)tick(*server);
        require(server->connection_count() == 0 && tick(*server, 0).stop == LoopStop::idle,
            "EOF cleanup and idle no OUT spin");
    }
    auto server = loop();
    auto peer = client(*server);
    (void)tick(*server);
    // 两个错误类别使用固定独立期望，而非生产helper自证。
    send(peer.get(), wire(3, 5, "{]") + wire(3, 6) + wire(1, 7, "[]"));
    const auto expected = wire(0xffff, 5, "{\"message\":\"invalid request\"}", 1)
                          + wire(0xffff, 6, "{\"message\":\"invalid request\"}", 1)
                          + wire(0xffff, 7, "{\"message\":\"invalid request\"}", 1);
    require(receive(*server, peer.get(), expected.size()) == expected,
        "Body and unsupported business errors preserve IDs");
    std::string deep(65, '[');
    deep += '0';
    deep.append(65, ']');
    send(peer.get(), wire(3, 8, "{\"x\":" + deep + "}"));
    const auto depth = wire(0xffff, 8, "{\"message\":\"resource limit\"}", 6);
    require(receive(*server, peer.get(), depth.size()) == depth,
        "local depth refusal has RESOURCE_LIMIT");
}
void budgets_and_capacity() {
    ProtocolLoopConfig config;
    config.max_connections = 1;
    config.read_byte_budget = 3;
    config.write_byte_budget = 1;
    config.io_attempt_budget = 1;
    config.accept_attempt_budget = 1;
    auto server = loop(config);
    auto first = client(*server);
    auto step = tick(*server);
    require(step.accept_attempts == 1 && step.accepted == 1 && server->listener_paused(),
        "capacity suspends listener");
    auto second = client(*server);
    send(first.get(), wire(1, 9));
    std::string got;
    std::array<char, 64> bytes{};
    for (unsigned int i = 0; got.size() < 26 && i < 128; ++i) {
        step = tick(*server);
        require(step.read_bytes <= 3 && step.write_bytes <= 1 && step.read_attempts <= 1
                    && step.write_attempts <= 1 && step.requests <= 1 && step.accepted == 0,
            "per-turn double budgets and no full-capacity accept");
        const auto n = ::recv(first.get(), bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (n > 0)
            got.append(bytes.data(), static_cast<std::size_t>(n));
    }
    require(got == wire(0x8001, 9), "one byte writes form exactly one frame");
    require(!first.close(), "first client leaves");
    for (unsigned int i = 0; i < 16 && server->connection_count() != 0; ++i)
        (void)tick(*server);
    require(server->connection_count() == 0 && !server->listener_paused(),
        "free slot resumes listener");
    step = tick(*server);
    require(step.accepted == 1 && server->connection_count() == 1,
        "pending second client admitted after resume");
    send(second.get(), wire(1, 10));
    require(receive(*server, second.get(), 26) == wire(0x8001, 10), "resumed client works");
    require(!server->close() && server->port() == 0 && server->connection_count() == 0
                && !server->listener_paused() && server->run_once(-2).stop == LoopStop::closed
                && !server->close(),
        "idempotent immediate close");
}
void fairness() {
    ProtocolLoopConfig config;
    config.max_connections = 2;
    config.write_byte_budget = 1;
    auto server = loop(config);
    auto slow = client(*server);
    auto ready = client(*server);
    (void)tick(*server);
    const auto request = wire(1, 31);
    send(slow.get(), request.substr(0, 5));
    send(ready.get(), wire(1, 32));
    require(
        receive(*server, ready.get(), 26) == wire(0x8001, 32) && server->connection_count() == 2,
        "fragmented client does not block another connection's partial writes");
    char byte = 0;
    require(
        ::recv(slow.get(), &byte, 1, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
        "incomplete client has no speculative response");
    send(slow.get(), request.substr(5));
    require(
        receive(*server, slow.get(), 26) == wire(0x8001, 31), "slow client eventually completes");
}
void terminal_inputs() {
    for (const auto mode : {0, 1, 2}) {
        auto server = loop();
        auto peer = client(*server);
        (void)tick(*server);
        auto bytes = wire(1, 4);
        if (mode == 0)
            bytes[0] = 'X';
        if (mode == 1)
            bytes.resize(13);
        if (mode == 2) {
            const linger reset{1, 0};
            require(::setsockopt(peer.get(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0,
                "RST setup");
            require(!peer.close(), "peer RST");
        } else {
            send(peer.get(), bytes);
            require(::shutdown(peer.get(), SHUT_WR) == 0, "damaged stream FIN");
        }
        for (unsigned int i = 0; server->connection_count() && i < 16; ++i)
            (void)tick(*server);
        require(server->connection_count() == 0 && server->run_once(0).stop == LoopStop::idle,
            "bad head/truncation/RST closes only client");
    }
    ProtocolLoopConfig tiny;
    tiny.max_body_bytes = 2;
    auto server = loop(tiny);
    auto peer = client(*server);
    (void)tick(*server);
    send(peer.get(), wire(3, 1));
    (void)tick(*server);
    require(
        server->connection_count() == 0, "error body cannot fit: close without recursive response");
    auto healthy = client(*server);
    (void)tick(*server);
    send(healthy.get(), wire(1, 2));
    require(receive(*server, healthy.get(), 26) == wire(0x8001, 2),
        "minimum budget still supports PING");
    const auto invalid = server->run_once(-2);
    require(invalid.stop == LoopStop::invalid_timeout && invalid.error.value() == EINVAL
                && server->connection_count() == 1,
        "invalid timeout preserves loop");
}
void listener_zero() {
    const auto child = ::fork();
    require(child >= 0, "isolated fd0 child");
    if (child == 0) {
        (void)::close(STDIN_FILENO);
        auto server = loop();
        require(::fcntl(0, F_GETFD) >= 0, "listener may own fd zero");
        auto peer = client(*server);
        (void)tick(*server);
        send(peer.get(), wire(1, 90));
        require(receive(*server, peer.get(), 26) == wire(0x8001, 90),
            "token zero listener distinct from fd zero");
        require(!server->close() && ::fcntl(0, F_GETFD) < 0 && errno == EBADF,
            "fd zero closed exactly as a valid resource");
        server.reset();
        (void)peer.close();
        ::_exit(0);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "isolated child succeeds");
}
} // namespace
int main() {
    protocol();
    budgets_and_capacity();
    fairness();
    terminal_inputs();
    listener_zero();
    std::cout << "parent_checks=" << checks << " isolated_children=1 failures=0\n";
}
