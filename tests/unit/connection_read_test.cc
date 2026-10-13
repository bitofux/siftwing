/*
 * PROJECT : SIFTWING
 * FILE    : connection_read_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 独立线字节、真实流和loopback验证分片、门禁、EOF及资源生命
 */
#include "siftwing/net/connection.hpp"
#include "siftwing/net/listener.hpp"
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
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
void require(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        std::cerr << "connection read failed: " << message << '\n';
        std::exit(1);
    }
}
struct Pair {
    UniqueFd reader;
    UniqueFd writer;
};
Pair pair() {
    int descriptors[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors) == 0,
        "real stream fixture");
    return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}
// 独立按协议偏移构造，不调用生产encoder，不让同一bug同时影响输入与期望。
std::string wire(std::uint16_t type = 1,
    std::string_view body = "{}",
    std::uint64_t id = 0x0102030405060708ULL) {
    std::string bytes(24, '\0');
    bytes.replace(0, 4, "SFTW");
    bytes[4] = 1;
    bytes[5] = 24;
    const auto put = [&](std::size_t offset, unsigned int width, std::uint64_t value) {
        for (unsigned int i = 0; i < width; ++i)
            bytes[offset + i] = static_cast<char>((value >> ((width - i - 1) * 8)) & 255U);
    };
    put(6, 2, type);
    put(12, 4, body.size());
    put(16, 8, id);
    bytes.append(body);
    return bytes;
}
void send_all(int fd, std::string_view bytes) {
    for (unsigned int i = 0; !bytes.empty() && i < 100; ++i) {
        const auto sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        } else {
            require(sent == -1 && (errno == EAGAIN || errno == EINTR), "bounded send fixture");
            pollfd event{fd, POLLOUT, 0};
            require(::poll(&event, 1, 100) >= 0, "fixture write wait");
        }
    }
    require(bytes.empty(), "fixture fully sent");
}
void valid(const ReadResult &result, std::uint64_t id = 0x0102030405060708ULL) {
    require(result.stop == ReadStop::request && result.request && !result.body_error
                && !result.header_error && !result.error && result.request->request_id == id,
        "owning valid request");
}
void splits_and_gate() {
    const auto bytes = wire();
    // 穷举每个帧头/Body分片边界，半帧不得消费，下一轮完整时恰好消费一帧。
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader), 128);
        send_all(sockets.writer.get(), std::string_view(bytes).substr(0, split));
        auto first = connection.read_request();
        if (split < bytes.size()) {
            require(first.stop == ReadStop::would_block && !first.request && first.bytes == split
                        && connection.buffered_bytes() == split,
                "partial frame preserved");
            send_all(sockets.writer.get(), std::string_view(bytes).substr(split));
            first = connection.read_request();
        }
        valid(first);
        require(first.request->body == "{}" && connection.request_pending()
                    && connection.buffered_bytes() == 0 && !sockets.reader,
            "request transfer sets gate");
        auto gated = connection.read_request(0, 0);
        require(gated.stop == ReadStop::request_pending && gated.attempts == 0 && gated.bytes == 0,
            "gate before zero budget");
        require(connection.finish_request() && !connection.finish_request(), "finish gate once");
        require(
            connection.read_request(0, 1).stop == ReadStop::invalid_budget, "zero bytes refused");
        require(connection.read_request(1, 0).stop == ReadStop::invalid_budget,
            "zero attempts refused");
    }
    auto sockets = pair();
    Connection connection(std::move(sockets.reader), 256);
    send_all(sockets.writer.get(), wire(1, "{}", 1) + wire(2, "{}", 2) + wire(3, "{}", 3));
    auto first = connection.read_request();
    valid(first, 1);
    require(connection.buffered_bytes() == 52, "coalesced suffix retained");
    require(connection.finish_request(), "finish first");
    auto second = connection.read_request(1, 1);
    valid(second, 2);
    require(second.bytes == 0 && second.attempts == 0, "cached frame needs no kernel readiness");
    require(connection.finish_request(), "finish second");
    auto third = connection.read_request();
    valid(third, 3);
    require(third.attempts == 0 && third.request->message_type == MessageType::search_request,
        "third cached request in order");
    require(!connection.close() && !connection.close(), "idempotent close");
    require(third.request->body == "{}" && connection.get() == -1 && !connection.request_pending()
                && connection.buffered_bytes() == 0
                && connection.read_request().stop == ReadStop::closed,
        "delivered request outlives connection");
}
void budgets_and_eof() {
    for (const bool by_bytes : {true, false}) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader), 2);
        send_all(sockets.writer.get(), wire());
        auto partial = connection.read_request(by_bytes ? 5U : 1U, by_bytes ? 16U : 1U);
        require(partial.stop == ReadStop::budget_exhausted && partial.attempts == 1
                    && partial.bytes == (by_bytes ? 5U : 1U) && !partial.error,
            "finite read budget preserves prefix");
        auto result = connection.read_request(26 - partial.bytes, 1);
        valid(result);
        require(result.attempts == 1, "request wins final budget boundary");
    }
    // EOF前的完整帧先交付；门禁期间仍可通过借用fd写回，FIN不是业务完成。
    auto sockets = pair();
    Connection connection(std::move(sockets.reader), 128);
    send_all(sockets.writer.get(), wire() + wire(4, "{}", 9));
    require(::shutdown(sockets.writer.get(), SHUT_WR) == 0, "real peer half close");
    valid(connection.read_request());
    require(connection.get() >= 0 && connection.read_request().stop == ReadStop::request_pending,
        "half close does not discard delivered request");
    send_all(connection.get(), "x");
    char byte = 0;
    require(::recv(sockets.writer.get(), &byte, 1, 0) == 1 && byte == 'x',
        "half closed peer reads reply");
    require(connection.finish_request(), "first half close reply done");
    valid(connection.read_request(), 9);
    require(connection.finish_request(), "second reply done");
    auto end = connection.read_request();
    require(end.stop == ReadStop::peer_eof && end.attempts == 1 && end.bytes == 0
                && connection.get() == -1 && connection.read_request().attempts == 0,
        "clean EOF closes once");
    for (const std::size_t length : {1U, 23U, 24U, 25U}) {
        auto incomplete = pair();
        Connection truncated(std::move(incomplete.reader));
        send_all(incomplete.writer.get(), std::string_view(wire()).substr(0, length));
        require(::shutdown(incomplete.writer.get(), SHUT_WR) == 0, "truncation FIN fixture");
        auto result = truncated.read_request();
        require(result.stop == ReadStop::truncated && result.bytes == length && !result.request
                    && truncated.get() == -1 && truncated.buffered_bytes() == 0,
            "EOF partial header or Body never published");
    }
}
void rejection() {
    // 头部破坏可在没有Body时拒绝；每类独立期望来自协议，而非decoder回读。
    struct Case {
        std::size_t offset;
        unsigned char value;
        DecodeError error;
    };
    for (const Case item : {Case{0, 'X', DecodeError::magic},
             {4, 2, DecodeError::version},
             {5, 25, DecodeError::header_size},
             {7, 99, DecodeError::message_type},
             {9, 1, DecodeError::flags},
             {11, 1, DecodeError::status},
             {15, 1, DecodeError::body_length},
             {6, 128, DecodeError::direction}}) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader));
        auto bytes = wire();
        bytes[item.offset] = static_cast<char>(item.value);
        send_all(sockets.writer.get(), std::string_view(bytes).substr(0, 24));
        auto result = connection.read_request(24, 1);
        require(result.stop == ReadStop::header_error && result.header_error == item.error
                    && !result.request && connection.get() == -1 && result.bytes == 24,
            "invalid header closes before budget or Body wait");
        auto again = connection.read_request();
        require(
            again.header_error == item.error && again.attempts == 0, "header first cause cached");
    }
    for (const bool zero_id : {true, false}) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader), 2);
        auto bytes = zero_id ? wire(1, "{}", 0) : wire(3, "{  }");
        send_all(sockets.writer.get(), bytes);
        auto result = connection.read_request();
        require(
            result.header_error == (zero_id ? DecodeError::request_id : DecodeError::body_length)
                && connection.get() == -1,
            "id and local length refusal");
    }
    struct BodyCase {
        std::string body;
        ProtocolError error;
        std::uint16_t type;
    };
    for (const auto &item : {BodyCase{"[]", ProtocolError::json_object, 3},
             {"{x}", ProtocolError::json, 3},
             {std::string("\xff{}", 3), ProtocolError::utf8, 3},
             {"{ }", ProtocolError::heartbeat, 1},
             {"{\"a\":" + std::string(64, '[') + "0" + std::string(64, ']') + "}",
                 ProtocolError::json_depth,
                 3}}) {
        auto sockets = pair();
        Connection connection(std::move(sockets.reader));
        send_all(sockets.writer.get(), wire(item.type, item.body, 51) + wire(1, "{}", 52));
        auto result = connection.read_request();
        require(result.stop == ReadStop::request && result.body_error == item.error
                    && result.request->request_id == 51 && result.request->body == item.body
                    && connection.request_pending() && connection.get() >= 0,
            "trusted header Body refusal gated, no business request");
        auto error = make_error_response(*result.request, ErrorStatus::invalid_request);
        require(std::holds_alternative<Frame>(error) && std::get<Frame>(error).request_id == 51,
            "trusted error response context");
        require(connection.finish_request(), "error reply completion gate");
        valid(connection.read_request(), 52);
    }
}
void large_and_lifetime() {
    auto sockets = pair();
    Connection connection(std::move(sockets.reader));
    const auto body = "{\"pad\":\"" + std::string(frame_max_body_bytes - 10, 'x') + "\"}";
    const auto bytes = wire(3, body, UINT64_MAX);
    std::size_t sent = 0;
    ReadResult result;
    // 大帧必须跨多个真实短读；同时泵写入和读取，避免用一次send成功假定整帧可用。
    for (unsigned int i = 0; i < 10000 && result.stop != ReadStop::request; ++i) {
        if (sent < bytes.size()) {
            const auto count = ::send(
                sockets.writer.get(), bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
            if (count > 0)
                sent += static_cast<std::size_t>(count);
            else
                require(count == -1 && (errno == EAGAIN || errno == EINTR), "large send bounded");
        }
        result = connection.read_request(8192, 3);
        require(result.bytes <= 8192 && result.attempts <= 3, "large dual budgets");
    }
    valid(result, UINT64_MAX);
    require(
        sent == bytes.size() && result.request->body == body && connection.buffered_bytes() == 0,
        "exact maximum Body and uint64 id");
    int owned = -1;
    try {
        auto temporary = pair();
        owned = temporary.reader.get();
        Connection scoped(std::move(temporary.reader));
        throw 7;
    } catch (int) {
    }
    require(::fcntl(owned, F_GETFD) == -1 && errno == EBADF, "RAII exception unwinding closes fd");
    for (const std::size_t limit : {0U, 1U, 1048577U}) {
        auto invalid = pair();
        const int fd = invalid.reader.get();
        bool refused = false;
        try {
            Connection bad(std::move(invalid.reader), limit);
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        require(refused && invalid.reader.get() == fd && ::fcntl(fd, F_GETFD) >= 0,
            "configuration failure does not steal caller fd");
    }
    auto invalid = pair();
    bool refused = false;
    try {
        Connection bad(std::move(invalid.reader), 2, 27);
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    require(refused && invalid.reader, "initial capacity failure source preserved");
    UniqueFd empty;
    refused = false;
    try {
        Connection bad(std::move(empty));
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    require(refused && !empty, "empty fd refused");
}
void tcp() {
    for (const auto family : {IpFamily::v4, IpFamily::v6}) {
        auto listen =
            Listener::create({family, family == IpFamily::v4 ? "127.0.0.1" : "::1", 0, 8, true});
        require(std::holds_alternative<Listener>(listen), "real TCP listener");
        auto &listener = std::get<Listener>(listen);
        auto socket = open_tcp_socket(family);
        require(std::holds_alternative<UniqueFd>(socket), "real TCP client");
        auto &client = std::get<UniqueFd>(socket);
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
        const int connected = ::connect(client.get(), address, size);
        require(connected == 0 || (connected == -1 && errno == EINPROGRESS), "loopback connect");
        pollfd event{listener.get(), POLLIN, 0};
        require(::poll(&event, 1, 2000) == 1, "bounded listener readiness");
        auto accepted = listener.accept_one();
        require(std::holds_alternative<UniqueFd>(accepted), "real accept into Connection");
        Connection connection(std::move(std::get<UniqueFd>(accepted)), 128);
        send_all(client.get(), wire(4, "{\"q\":\"猫\"}", 8));
        event = {connection.get(), POLLIN, 0};
        require(::poll(&event, 1, 2000) == 1, "bounded read readiness");
        auto result = connection.read_request();
        valid(result, 8);
        require(result.request->body == "{\"q\":\"猫\"}", "TCP UTF8 Body preserved");
    }
}
} // namespace
int main() {
    static_assert(
        !std::is_copy_constructible_v<Connection> && !std::is_move_constructible_v<Connection>);
    static_assert(std::is_nothrow_move_constructible_v<ReadResult>);
    splits_and_gate();
    budgets_and_eof();
    rejection();
    large_and_lifetime();
    tcp();
    // fd0在隔离子进程实际取得，不能为了测试关闭父进程stdin。
    const auto child = ::fork();
    require(child >= 0, "fd0 child created");
    if (child == 0) {
        auto sockets = pair();
        require(::dup2(sockets.reader.get(), 0) == 0, "real reader fd0");
        if (sockets.reader.get() != 0)
            require(!sockets.reader.close(), "close pre-dup fixture fd");
        else
            (void)sockets.reader.release();
        Connection connection(UniqueFd(0), 2);
        send_all(sockets.writer.get(), wire());
        valid(connection.read_request());
        require(connection.get() == 0 && !connection.close() && ::fcntl(0, F_GETFD) == -1,
            "valid fd0 owned and closed");
        ::_exit(0);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fd0 child succeeded");
    std::cout << "parent_checks=" << checks << " isolated_children=1 failures=0\n";
}
