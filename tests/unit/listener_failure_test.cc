/*
 * PROJECT : SIFTWING
 * FILE    : listener_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 测试专用wrap验证创建失败清理、预算、原errno及部分成功所有权
 */
#include "siftwing/net/listener.hpp"
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0, socket_calls = 0, option_calls = 0, bind_calls = 0, listen_calls = 0,
             name_calls = 0, close_calls = 0, accept_calls = 0;
int stage = 0, injected = 0, created_fd = -1;
bool malformed_name = false;
int observed_flags = 0, observed_listener = -1;
bool null_peer = false;
std::array<int, 16> schedule{};
std::size_t schedule_size = 0, cursor = 0;
void require(bool value, const char *message) {
    ++checks;
    if (!value) {
        std::cerr << "listener injection failed: " << message << '\n';
        std::exit(1);
    }
}
bool error_is(std::error_code error, int value) {
    return error.value() == value && error.category() == std::generic_category();
}
int fail() {
    errno = injected;
    return -1;
}
void plan(std::initializer_list<int> values) {
    require(values.size() <= schedule.size(), "fixture schedule capacity");
    schedule_size = values.size();
    cursor = 0;
    std::size_t i = 0;
    for (const int value : values)
        schedule[i++] = value;
}
Listener listener() {
    auto result = Listener::create({});
    require(std::holds_alternative<Listener>(result), "real listener fixture");
    return std::move(std::get<Listener>(result));
}
UniqueFd connect_to(std::uint16_t port) {
    auto result = open_tcp_socket(IpFamily::v4);
    require(std::holds_alternative<UniqueFd>(result), "client fixture");
    auto client = std::move(std::get<UniqueFd>(result));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int status =
        ::connect(client.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address));
    require(status == 0 || (status == -1 && errno == EINPROGRESS), "fixture connect starts");
    pollfd event{client.get(), POLLOUT, 0};
    require(::poll(&event, 1, 2000) == 1, "fixture bounded connect");
    int error = -1;
    socklen_t size = sizeof(error);
    require(::getsockopt(client.get(), SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0,
        "fixture connected");
    return client;
}
} // namespace
extern "C" {
int __real_socket(int, int, int);
int __real_setsockopt(int, int, int, const void *, socklen_t);
int __real_bind(int, const sockaddr *, socklen_t);
int __real_listen(int, int);
int __real_getsockname(int, sockaddr *, socklen_t *);
int __real_accept4(int, sockaddr *, socklen_t *, int);
int __real_close(int);
int __wrap_socket(int domain, int type, int protocol) {
    ++socket_calls;
    if (stage == 1)
        return fail();
    created_fd = __real_socket(domain, type, protocol);
    return created_fd;
}
int __wrap_setsockopt(int fd, int level, int name, const void *value, socklen_t size) {
    ++option_calls;
    if (stage == 2)
        return fail();
    return __real_setsockopt(fd, level, name, value, size);
}
int __wrap_bind(int fd, const sockaddr *address, socklen_t size) {
    ++bind_calls;
    if (stage == 3)
        return fail();
    return __real_bind(fd, address, size);
}
int __wrap_listen(int fd, int backlog) {
    ++listen_calls;
    if (stage == 4)
        return fail();
    return __real_listen(fd, backlog);
}
int __wrap_getsockname(int fd, sockaddr *address, socklen_t *size) {
    ++name_calls;
    if (stage == 5)
        return fail();
    const int result = __real_getsockname(fd, address, size);
    if (malformed_name)
        *size = 0;
    return result;
}
int __wrap_close(int fd) {
    ++close_calls;
    return __real_close(fd);
}
int __wrap_accept4(int fd, sockaddr *peer, socklen_t *size, int flags) {
    ++accept_calls;
    observed_flags = flags;
    observed_listener = fd;
    null_peer = peer == nullptr && size == nullptr;
    if (cursor < schedule_size) {
        const int error = schedule[cursor++];
        if (error) {
            errno = error;
            return -1;
        }
    }
    return __real_accept4(fd, peer, size, flags);
}
}
int main() {
    // 预检独立验证：不依赖errno遗留值或实现的内部断言。
    for (const ListenConfig config :
        {ListenConfig{static_cast<IpFamily>(999), "127.0.0.1", 0, 8, false},
            ListenConfig{IpFamily::v4, "", 0, 8, false},
            ListenConfig{IpFamily::v4, std::string_view("127.0.0.1\0x", 11), 0, 8, false},
            ListenConfig{IpFamily::v4, "localhost", 0, 8, false},
            ListenConfig{IpFamily::v4, "::1", 0, 8, false},
            ListenConfig{IpFamily::v6, "127.0.0.1", 0, 8, false},
            ListenConfig{IpFamily::v6, "fe80::1%lo", 0, 8, false},
            ListenConfig{IpFamily::v4, "127.0.0.1", 0, 0, false},
            ListenConfig{IpFamily::v4, "127.0.0.1", 0, -1, false},
            ListenConfig{
                IpFamily::v6, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 0, 8, false}}) {
        const auto before = socket_calls;
        auto result = Listener::create(config);
        require(std::holds_alternative<std::error_code>(result)
                    && error_is(std::get<std::error_code>(result), EINVAL)
                    && socket_calls == before,
            "invalid config no socket syscall");
    }
    // 各阶段每种原错误均立即停止；后续阶段调用数不变，临时拥有者恰好关闭一次。
    for (const int error : {EINTR, EMFILE, ENFILE, ENOMEM, ENOBUFS, EACCES, EADDRINUSE, EINVAL}) {
        for (const int failure_stage : {1, 2, 3, 4, 5}) {
            stage = failure_stage;
            injected = error;
            const std::array<unsigned int, 6> before{
                socket_calls, option_calls, bind_calls, listen_calls, name_calls, close_calls};
            auto result = Listener::create({IpFamily::v4, "127.0.0.1", 0, 8, true});
            require(std::holds_alternative<std::error_code>(result)
                        && error_is(std::get<std::error_code>(result), error),
                "creation original errno");
            const std::array<unsigned int, 6> after{
                socket_calls, option_calls, bind_calls, listen_calls, name_calls, close_calls};
            for (std::size_t i = 0; i < 5; ++i)
                require(
                    after[i] == before[i] + (i < static_cast<std::size_t>(failure_stage) ? 1U : 0U),
                    "one syscall per reached phase no hidden retry");
            require(close_calls == before[5] + (failure_stage == 1 ? 0U : 1U),
                "created resource closed exactly once");
            if (failure_stage != 1)
                require(::fcntl(created_fd, F_GETFD) == -1 && errno == EBADF,
                    "failed creation really closed");
        }
    }
    stage = 0;
    // IPv6首个强制V6ONLY配置失败同样释放，而不是退化为隐式双栈。
    stage = 2;
    injected = ENOPROTOOPT;
    auto refused_v6 = Listener::create({IpFamily::v6, "::1", 0, 8, false});
    require(std::holds_alternative<std::error_code>(refused_v6)
                && error_is(std::get<std::error_code>(refused_v6), ENOPROTOOPT),
        "V6ONLY failure forwarded");
    require(::fcntl(created_fd, F_GETFD) == -1 && errno == EBADF, "v6 failure closes resource");
    stage = 0;
    malformed_name = true;
    auto malformed = Listener::create({});
    require(std::holds_alternative<std::error_code>(malformed)
                && error_is(std::get<std::error_code>(malformed), EIO),
        "malformed getsockname length refused");
    malformed_name = false;
    // 非NUL结尾的string_view输入在创建期间有界复制，不读取view以外字符。
    auto copied =
        Listener::create({IpFamily::v4, std::string_view("127.0.0.1junk", 9), 0, 8, false});
    require(std::holds_alternative<Listener>(copied), "numeric string_view bounded copy");
    auto acceptor = listener();
    for (const int error : {EINTR,
             ECONNABORTED,
             ENETDOWN,
             EPROTO,
             ENOPROTOOPT,
             EHOSTDOWN,
             ENONET,
             EHOSTUNREACH,
             EOPNOTSUPP,
             ENETUNREACH,
             EAGAIN,
             EMFILE,
             ENFILE,
             ENOBUFS,
             ENOMEM,
             EBADF}) {
        plan({error});
        const auto before = accept_calls;
        auto result = acceptor.accept_one();
        require(std::holds_alternative<std::error_code>(result)
                    && error_is(std::get<std::error_code>(result), error)
                    && accept_calls == before + 1,
            "single accept original error no retry");
        require(observed_flags == (SOCK_NONBLOCK | SOCK_CLOEXEC) && null_peer
                    && observed_listener == acceptor.get(),
            "accept4 exact flags/null peer/listener");
    }
    std::array<UniqueFd, 3> outputs;
    for (const int error : {EINTR,
             ECONNABORTED,
             ENETDOWN,
             EPROTO,
             ENOPROTOOPT,
             EHOSTDOWN,
             ENONET,
             EHOSTUNREACH,
             EOPNOTSUPP,
             ENETUNREACH}) {
        plan({error, error, error});
        const auto before = accept_calls;
        auto batch = acceptor.accept_into(outputs.data(), outputs.size(), 3);
        require(batch.stop == AcceptStop::budget_exhausted && batch.attempts == 3
                    && batch.accepted == 0 && !batch.error && accept_calls == before + 3,
            "every retryable error charged to finite attempts");
    }
    for (const int error : {EMFILE, ENFILE, ENOBUFS, ENOMEM, EBADF, EINVAL}) {
        auto client = connect_to(acceptor.port());
        plan({0, error, 0});
        const auto before = accept_calls;
        const auto closed = close_calls;
        auto batch = acceptor.accept_into(outputs.data(), outputs.size(), 8);
        require(batch.stop == AcceptStop::error && batch.attempts == 2 && batch.accepted == 1
                    && error_is(batch.error, error) && accept_calls == before + 2,
            "terminal error stops after partial success");
        require(outputs[0] && !outputs[1] && ::fcntl(outputs[0].get(), F_GETFD) >= 0
                    && ::fcntl(acceptor.get(), F_GETFD) >= 0 && close_calls == closed,
            "partial success and listener remain caller-owned");
        require(!outputs[0].close(), "fixture releases partial accepted fd");
    }
    auto client = connect_to(acceptor.port());
    plan({EINTR, ECONNABORTED, 0, EAGAIN});
    auto mixed = acceptor.accept_into(outputs.data(), outputs.size(), 4);
    require(mixed.stop == AcceptStop::drained && mixed.accepted == 1 && mixed.attempts == 4
                && error_is(mixed.error, EAGAIN),
        "retry/success/drain mixed sequence");
    require(!outputs[0].close(), "release mixed success");
    plan({EINTR, EAGAIN});
    auto interrupted = acceptor.accept_into(outputs.data(), outputs.size(), 3);
    require(interrupted.stop == AcceptStop::drained && interrupted.attempts == 2
                && interrupted.accepted == 0,
        "EINTR then drained queue");
    const auto before = accept_calls;
    Listener empty;
    require(
        error_is(std::get<std::error_code>(empty.accept_one()), EBADF), "empty single precheck");
    require(error_is(empty.accept_into(nullptr, 0, 0).error, EBADF), "empty batch fd priority");
    for (const auto batch : {acceptor.accept_into(nullptr, 1, 1),
             acceptor.accept_into(outputs.data(), 0, 1),
             acceptor.accept_into(outputs.data(), 1, 0)})
        require(batch.attempts == 0 && error_is(batch.error, EINVAL), "invalid batch precheck");
    outputs[1] = UniqueFd(::dup(acceptor.get()));
    const int protected_fd = outputs[1].get();
    auto occupied = acceptor.accept_into(outputs.data(), outputs.size(), 3);
    require(occupied.attempts == 0 && error_is(occupied.error, EINVAL)
                && outputs[1].get() == protected_fd && accept_calls == before,
        "occupied prospective slot zero syscall and preserved");
    // 预算只可能填第一槽；非目标尾槽已有资源时无需拒绝或覆盖它。
    plan({EINTR});
    auto limited = acceptor.accept_into(outputs.data(), outputs.size(), 1);
    require(limited.stop == AcceptStop::budget_exhausted && limited.attempts == 1
                && outputs[1].get() == protected_fd,
        "untouched suffix beyond possible success prefix");
    std::cout
        << "checks=" << checks
        << " failures=0 test_only_wrap=socket,setsockopt,bind,listen,getsockname,accept4,close\n";
}
