/*
 * PROJECT : SIFTWING
 * FILE    : protocol_loop_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 测试专用系统调用与分配注入验证Loop调度、首因及资源清理
 */
#include "siftwing/net/protocol_loop.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace siftwing::net;
unsigned int checks = 0, waits = 0, ctls = 0, accepts = 0, sends = 0, receives = 0, closes = 0,
             sockets = 0, pollers = 0;
int listener_fd = -1, client_fd = -1, epoll_fd = -1, last_accepted_fd = -1;
unsigned int accept_prefix = 0;
std::uint64_t client_token = 0;
std::uint32_t client_mask = 0, listener_mask = 0;
int ctl_operation = 0, ctl_errno = 0, wait_errno = 0, accept_errno = 0, poller_errno = 0;
bool ctl_listener = false, force_events = false, invalid_count = false;
std::array<std::uint64_t, 4> event_tokens{};
int event_count = 0, last_timeout = -99;
std::array<int, 4> send_plan{};
std::size_t send_cursor = 0, send_count = 0;
std::string recv_plan;
std::size_t recv_cursor = 0;
std::size_t fail_new = 0, new_calls = 0;
bool allocation_scope = false;
int fail_close_fd = -1;
void require(bool value, const char *message) {
    ++checks;
    if (!value) {
        std::cerr << "loop injection failed: " << message << '\n';
        std::exit(1);
    }
}
void reset() {
    ctl_operation = 0;
    ctl_errno = 0;
    wait_errno = 0;
    accept_errno = 0;
    accept_prefix = 0;
    poller_errno = 0;
    force_events = false;
    invalid_count = false;
    send_cursor = 0;
    send_count = 0;
    recv_plan.clear();
    recv_cursor = 0;
    fail_new = 0;
    allocation_scope = false;
    fail_close_fd = -1;
}
std::string wire(std::uint64_t id) {
    std::string bytes("SFTW\x01\x18\x00\x01\x00\x00\x00\x00\x00\x00\x00\x02", 16);
    for (unsigned int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<char>((id >> (8 * (7 - i))) & 255U));
    bytes += "{}";
    return bytes;
}
std::unique_ptr<ProtocolLoop> loop(ProtocolLoopConfig config = {}) {
    reset();
    auto created = ProtocolLoop::create({}, config);
    require(std::holds_alternative<std::unique_ptr<ProtocolLoop>>(created), "loop setup");
    return std::move(std::get<std::unique_ptr<ProtocolLoop>>(created));
}
UniqueFd client(ProtocolLoop &server, bool adopt = true) {
    auto result = open_tcp_socket(IpFamily::v4);
    require(std::holds_alternative<UniqueFd>(result), "real client");
    auto fd = std::move(std::get<UniqueFd>(result));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int n = ::connect(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address));
    require(n == 0 || (n < 0 && errno == EINPROGRESS), "connect real listener");
    if (adopt) {
        const auto step = server.run_once(2000);
        require(step.accepted == 1, "accept client fixture");
    }
    return fd;
}
void events(std::initializer_list<std::uint64_t> tokens) {
    event_count = static_cast<int>(tokens.size());
    std::copy(tokens.begin(), tokens.end(), event_tokens.begin());
    force_events = true;
}
void request(std::uint64_t id, bool second = false) {
    recv_plan = wire(id);
    if (second)
        recv_plan += wire(id + 1);
    recv_cursor = 0;
    events({client_token});
}
void ctl_failure(int operation, bool listener, int error) {
    ctl_operation = operation;
    ctl_listener = listener;
    ctl_errno = error;
}
void arm(std::size_t nth) {
    new_calls = 0;
    fail_new = nth;
    allocation_scope = true;
}
void *allocate(std::size_t size) {
    if (allocation_scope && ++new_calls == fail_new)
        throw std::bad_alloc();
    if (void *p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
} // namespace
void *operator new(std::size_t size) {
    return allocate(size);
}
void *operator new[](std::size_t size) {
    return allocate(size);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}

extern "C" int __real_socket(int, int, int);
extern "C" int __wrap_socket(int domain, int type, int protocol) {
    ++sockets;
    return __real_socket(domain, type, protocol);
}
extern "C" int __real_epoll_create1(int);
extern "C" int __wrap_epoll_create1(int flags) {
    ++pollers;
    if (poller_errno) {
        errno = poller_errno;
        poller_errno = 0;
        return -1;
    }
    return __real_epoll_create1(flags);
}
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
extern "C" int __wrap_epoll_ctl(int epoll, int operation, int fd, epoll_event *event) {
    ++ctls;
    epoll_fd = epoll;
    const bool listener = event ? event->data.u64 == 0 : fd == listener_fd;
    if (event && listener)
        listener_fd = fd;
    if (ctl_operation == operation && listener == ctl_listener) {
        const int error = ctl_errno;
        ctl_operation = 0;
        errno = error;
        return -1;
    }
    const int result = __real_epoll_ctl(epoll, operation, fd, event);
    if (result == 0 && event) {
        if (listener)
            listener_mask = event->events;
        else {
            client_fd = fd;
            client_token = event->data.u64;
            client_mask = event->events;
        }
    }
    return result;
}
extern "C" int __real_epoll_wait(int, epoll_event *, int, int);
extern "C" int __wrap_epoll_wait(int epoll, epoll_event *output, int capacity, int timeout) {
    ++waits;
    last_timeout = timeout;
    if (wait_errno) {
        errno = wait_errno;
        wait_errno = 0;
        return -1;
    }
    if (invalid_count) {
        invalid_count = false;
        return capacity + 1;
    }
    if (force_events) {
        force_events = false;
        for (int i = 0; i < event_count; ++i) {
            output[i] = {};
            output[i].events = EPOLLIN | EPOLLOUT;
            output[i].data.u64 = event_tokens[static_cast<std::size_t>(i)];
        }
        return event_count;
    }
    return __real_epoll_wait(epoll, output, capacity, timeout);
}
extern "C" int __real_accept4(int, sockaddr *, socklen_t *, int);
extern "C" int __wrap_accept4(int fd, sockaddr *address, socklen_t *length, int flags) {
    ++accepts;
    if (accept_errno && accept_prefix == 0) {
        errno = accept_errno;
        return -1;
    }
    if (accept_prefix != 0)
        --accept_prefix;
    const int result = __real_accept4(fd, address, length, flags);
    if (result >= 0)
        last_accepted_fd = result;
    return result;
}
extern "C" ssize_t __real_recv(int, void *, std::size_t, int);
extern "C" ssize_t __wrap_recv(int fd, void *buffer, std::size_t size, int flags) {
    if (fd == client_fd) {
        ++receives;
        if (recv_cursor < recv_plan.size()) {
            const auto n = std::min(size, recv_plan.size() - recv_cursor);
            std::memcpy(buffer, recv_plan.data() + recv_cursor, n);
            recv_cursor += n;
            return static_cast<ssize_t>(n);
        }
    }
    return __real_recv(fd, buffer, size, flags);
}
extern "C" ssize_t __real_send(int, const void *, std::size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void *buffer, std::size_t size, int flags) {
    if (fd == client_fd) {
        ++sends;
        if (send_cursor < send_count) {
            const int action = send_plan[send_cursor++];
            if (action < 0) {
                errno = -action;
                return -1;
            }
            size = std::min(size, static_cast<std::size_t>(action));
        }
    }
    return __real_send(fd, buffer, size, flags);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
    ++closes;
    const int result = __real_close(fd);
    if (fd == fail_close_fd) {
        fail_close_fd = -1;
        errno = EINTR;
        return -1;
    }
    return result;
}

int main() {
    using namespace siftwing::net;
    static_assert(
        noexcept(std::declval<ProtocolLoop &>().run_once(0)), "step absorbs connection exceptions");
    reset();
    for (const auto field : {0, 1, 2, 3, 4, 5, 6}) {
        ProtocolLoopConfig config;
        if (field == 0)
            config.max_connections = 0;
        if (field == 1)
            config.max_connections = 1025;
        if (field == 2)
            config.max_body_bytes = 1;
        if (field == 3)
            config.accept_attempt_budget = 0;
        if (field == 4)
            config.read_byte_budget = 0;
        if (field == 5)
            config.write_byte_budget = frame_max_body_bytes + 25;
        if (field == 6)
            config.io_attempt_budget = 1025;
        const auto before = sockets + pollers + ctls;
        const auto result = ProtocolLoop::create({}, config);
        require(std::get<std::error_code>(result).value() == EINVAL
                    && sockets + pollers + ctls == before,
            "invalid config makes no syscall");
    }
    for (const auto stage : {0, 1, 2, 3}) {
        reset();
        const auto closed = closes;
        if (stage == 0) {
            arm(1);
        }
        if (stage == 1) {
            arm(2);
        }
        if (stage == 2) {
            ctl_failure(EPOLL_CTL_ADD, true, EACCES);
        }
        if (stage == 3) {
            poller_errno = ENFILE;
        }
        bool threw = false;
        int error = 0;
        try {
            auto result = ProtocolLoop::create();
            if (auto *e = std::get_if<std::error_code>(&result))
                error = e->value();
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        allocation_scope = false;
        require(
            stage <= 1 ? threw : error == (stage == 2 ? EACCES : ENFILE), "factory failure cause");
        require(closes - closed
                    == (stage == 0   ? 0U
                        : stage == 3 ? 1U
                                     : 2U),
            "factory resources close exactly once");
    }
    {
        auto server = loop();
        const auto calls = waits;
        require(server->run_once(-2).stop == LoopStop::invalid_timeout && waits == calls,
            "invalid timeout zero wait");
        wait_errno = EINTR;
        const auto interrupted = server->run_once(91);
        require(interrupted.stop == LoopStop::interrupted && interrupted.error.value() == EINTR
                    && waits == calls + 1 && server->port(),
            "wait EINTR no hidden retry");
        wait_errno = EBADF;
        const auto failed = server->run_once(0);
        const auto ended = waits + ctls + closes;
        require(failed.stop == LoopStop::failed && failed.error.value() == EBADF
                    && server->port() == 0 && server->run_once(0).error.value() == EBADF
                    && !server->close() && waits + ctls + closes == ended,
            "fatal wait cause and repeated zero calls");
    }
    for (const auto error : {EMFILE, ENFILE, ENOBUFS, ENOMEM}) {
        auto server = loop();
        events({0});
        accept_errno = error;
        const auto before = accepts;
        const auto step = server->run_once(0);
        reset();
        require(step.stop == LoopStop::failed && step.error.value() == error
                    && accepts == before + 1 && server->port() == 0,
            "accept resource error terminates, no busy retry");
    }
    {
        auto server = loop();
        auto existing = client(*server);
        auto pending = client(*server, false);
        accept_errno = EMFILE;
        accept_prefix = 1;
        fail_close_fd = client_fd;
        const auto before = closes;
        const auto step = server->run_once(2000);
        reset();
        require(step.stop == LoopStop::failed && step.error.value() == EMFILE && step.accepted == 1
                    && step.rejected_connections == 1 && step.dropped_connections == 1
                    && closes == before + 4,
            "accept success prefix plus resource failure closes incoming and existing resources; first cause survives close");
        require(::fcntl(last_accepted_fd, F_GETFD) < 0 && errno == EBADF,
            "unpublished accepted prefix fd really closed");
    }
    {
        ProtocolLoopConfig config;
        config.accept_attempt_budget = 3;
        auto server = loop(config);
        events({0, 0});
        accept_errno = EINTR;
        const auto before = accepts;
        const auto step = server->run_once(0);
        reset();
        require(
            step.stop == LoopStop::progress && step.accept_attempts == 3 && accepts == before + 3,
            "accept EINTR budget and duplicate listener event bound");
    }
    {
        auto server = loop();
        invalid_count = true;
        require(server->run_once(0).stop == LoopStop::failed,
            "impossible wait count cannot overrun event array");
    }
    {
        auto server = loop();
        ctl_failure(EPOLL_CTL_ADD, false, ENOSPC);
        auto opened = open_tcp_socket(IpFamily::v4);
        auto peer = std::move(std::get<UniqueFd>(opened));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server->port());
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int n = ::connect(peer.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        require(n == 0 || errno == EINPROGRESS, "ADD failure fixture connect");
        const auto step = server->run_once(2000);
        require(step.accepted == 1 && step.rejected_connections == 1
                    && step.first_connection_error.value() == ENOSPC
                    && server->connection_count() == 0 && server->port(),
            "client ADD error closes only new fd");
    }
    {
        auto server = loop();
        auto peer = client(*server);
        request(41, true);
        send_plan = {3, -EAGAIN, 0, 0};
        send_count = 2;
        const auto first = server->run_once(0);
        require(first.requests == 1 && first.write_bytes == 3 && first.completed_responses == 0
                    && client_mask == EPOLLOUT,
            "partial/EAGAIN output removes IN and RDHUP");
        events({client_token});
        const auto rest = server->run_once(0);
        require(rest.completed_responses == 1 && rest.write_bytes == 23
                    && client_mask == (EPOLLIN | EPOLLRDHUP),
            "drain restores reading");
        const auto before = receives;
        const auto next = server->run_once(-1);
        require(next.requests == 1 && next.completed_responses == 1 && receives == before
                    && last_timeout == 0,
            "cache runnable overrides infinite wait with zero timeout");
        std::array<char, 128> bytes{};
        std::string actual;
        for (unsigned int i = 0; actual.size() < 52 && i < 32; ++i) {
            pollfd ready{peer.get(), POLLIN, 0};
            require(::poll(&ready, 1, 100) >= 0, "finite peer readiness");
            const auto n = ::recv(peer.get(), bytes.data(), bytes.size(), MSG_DONTWAIT);
            if (n > 0)
                actual.append(bytes.data(), static_cast<std::size_t>(n));
            else
                require(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
                    "partial TCP response pending");
        }
        auto expected = wire(41) + wire(42);
        expected[6] = static_cast<char>(0x80);
        expected[32] = static_cast<char>(0x80);
        require(actual == expected, "real send prefixes form two independent complete PONG frames");
    }
    for (const auto operation : {EPOLL_CTL_MOD, EPOLL_CTL_DEL}) {
        ProtocolLoopConfig config;
        if (operation == EPOLL_CTL_DEL)
            config.max_body_bytes = 2;
        auto server = loop(config);
        auto peer = client(*server);
        const int fd = client_fd;
        if (operation == EPOLL_CTL_MOD) {
            request(1);
            send_plan = {-EAGAIN, 0, 0, 0};
            send_count = 1;
            ctl_failure(operation, false, EIO);
        } else {
            request(1);
            recv_plan[7] = 3;
            ctl_failure(operation, false, ENOENT);
        }
        fail_close_fd = fd;
        const auto step = server->run_once(2000);
        require(step.dropped_connections == 1 && server->connection_count() == 0 && server->port()
                    && ctl_operation == 0
                    && step.first_connection_error.value()
                           == (operation == EPOLL_CTL_MOD ? EIO : EPROTO),
            "client registration error local cleanup; close and DEL cannot overwrite primary cause");
        require(::fcntl(fd, F_GETFD) < 0 && errno == EBADF,
            "client really closed despite registration failure");
    }
    {
        auto server = loop();
        auto peer = client(*server);
        const auto old = client_token;
        require(!peer.close(), "old client close");
        (void)server->run_once(2000);
        require(server->connection_count() == 0, "old slot freed");
        auto current = client(*server);
        require(client_token != old, "token never reused with reused slot");
        const auto now_token = client_token;
        request(12);
        events({old});
        const auto before = receives + sends;
        require(server->run_once(0).requests == 0 && receives + sends == before,
            "stale token ignored instead of guessing fd");
        events({now_token, now_token});
        require(server->run_once(0).requests == 1, "duplicate client batch served once");
    }
    {
        ProtocolLoopConfig config;
        config.max_connections = 1;
        auto server = loop(config);
        ctl_failure(EPOLL_CTL_MOD, true, EIO);
        auto opened = open_tcp_socket(IpFamily::v4);
        auto peer = std::move(std::get<UniqueFd>(opened));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server->port());
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int n = ::connect(peer.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        require(n == 0 || errno == EINPROGRESS, "pause fixture");
        const auto failed = server->run_once(2000);
        require(failed.stop == LoopStop::failed && failed.error.value() == EIO
                    && server->connection_count() == 0,
            "listener pause MOD error closes whole loop");
    }
    {
        ProtocolLoopConfig config;
        config.max_connections = 1;
        auto server = loop(config);
        auto peer = client(*server);
        ctl_failure(EPOLL_CTL_MOD, true, ENOSPC);
        require(!peer.close(), "resume failure fixture");
        const auto step = server->run_once(2000);
        require(
            step.stop == LoopStop::failed && step.error.value() == ENOSPC && server->port() == 0,
            "listener resume error terminates loop");
    }
    {
        auto server = loop();
        auto peer = client(*server, false);
        arm(1);
        const auto before = closes;
        const auto step = server->run_once(2000);
        allocation_scope = false;
        require(step.stop == LoopStop::failed && step.error.value() == ENOMEM && step.accepted == 1
                    && step.rejected_connections == 1 && closes == before + 3
                    && server->port() == 0,
            "accept allocation failure closes unadopted fd and whole loop");
    }
    {
        auto server = loop();
        auto peer = client(*server);
        ctl_failure(EPOLL_CTL_DEL, false, ENOENT);
        fail_close_fd = client_fd;
        require(server->close().value() == ENOENT && server->connection_count() == 0
                    && server->port() == 0,
            "explicit close reports first cleanup error and closes all resources");
    }
    std::size_t allocation_points = 0;
    bool response_failure = false;
    {
        auto server = loop();
        auto peer = client(*server);
        request(10);
        arm(0);
        const auto step = server->run_once(0);
        allocation_points = new_calls;
        allocation_scope = false;
        require(step.completed_responses == 1 && allocation_points >= 2,
            "measure real read/response allocations");
    }
    for (std::size_t nth = 1; nth <= allocation_points; ++nth) {
        auto server = loop();
        auto peer = client(*server);
        request(10);
        arm(nth);
        const auto step = server->run_once(0);
        allocation_scope = false;
        require(step.dropped_connections == 1 && step.first_connection_error.value() == ENOMEM
                    && server->port() && server->connection_count() == 0,
            "each read/response allocation failure local cleanup");
        response_failure = response_failure || step.requests == 1;
    }
    require(
        response_failure, "cover failure after request publication during response construction");
    std::cout << "checks=" << checks << " allocation_points=" << allocation_points
              << " failures=0\n";
}
