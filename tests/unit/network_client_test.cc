/*
 * PROJECT : SIFTWING
 * FILE    : network_client_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 真实loopback交换的短I/O、EINTR、总期限及fd归还合同
 * IMPLEMENTATION : GNU静态符号包装仅干预调用线程；对端手写黄金线字节，不调用codec生成期望。
 */
#include "siftwing/net/client.hpp"
#include "siftwing/net/unique_fd.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
// 不修改对端线程的I/O；每种注入都核实际命中，避免只有名义故障覆盖。
thread_local int mode = 0, sends = 0, reads = 0, polls = 0, injected = 0;
int checks = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}
std::size_t descriptors() {
    std::size_t size = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++size;
    }
    return size;
}
}
extern "C" ssize_t __real_send(int, const void *, size_t, int);
extern "C" ssize_t __real_recv(int, void *, size_t, int);
extern "C" int __real_poll(pollfd *, nfds_t, int);
extern "C" ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
    if (mode) {
        ++sends;
        if (mode == 2) {
            ++injected;
            errno = EIO;
            return -1;
        }
        if (mode == 1 && sends <= 2) {
            ++injected;
            errno = sends == 1 ? EINTR : EAGAIN;
            return -1;
        }
        if (mode == 1) {
            ++injected;
            size = std::min(size, size_t{3});
        }
        if (!(::fcntl(fd, F_GETFL) & O_NONBLOCK) || !(::fcntl(fd, F_GETFD) & FD_CLOEXEC))
            throw std::runtime_error("client fd must be nonblocking/CLOEXEC");
    }
    return __real_send(fd, data, size, flags);
}
extern "C" ssize_t __wrap_recv(int fd, void *data, size_t size, int flags) {
    if (mode) {
        ++reads;
        if (mode == 3) {
            ++injected;
            errno = EIO;
            return -1;
        }
        if (mode == 5) {
            ++injected;
            errno = EAGAIN;
            return -1;
        }
        if (mode == 6) {
            ++injected;
            throw std::runtime_error("injected unknown exception");
        }
        if (mode == 1 && reads <= 2) {
            ++injected;
            errno = reads == 1 ? EINTR : EAGAIN;
            return -1;
        }
        if (mode == 1) {
            ++injected;
            size = std::min(size, size_t{2});
        }
    }
    return __real_recv(fd, data, size, flags);
}
extern "C" int __wrap_poll(pollfd *fds, nfds_t size, int timeout) {
    if (mode) {
        ++polls;
        if (mode == 4) {
            ++injected;
            errno = EIO;
            return -1;
        }
        if (mode == 5 || (mode == 1 && polls == 1)) {
            ++injected;
            if (mode == 5)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            errno = EINTR;
            return -1;
        }
    }
    return __real_poll(fds, size, timeout);
}
namespace {
void exchange_case(int selected) {
    using namespace siftwing::net;
    const auto before = descriptors();
    {
        UniqueFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        check(static_cast<bool>(listener), "listener socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
            "bind");
        socklen_t length = sizeof(address);
        check(::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address), &length) == 0,
            "port");
        check(::listen(listener.get(), 1) == 0, "listen");
        // 黄金PONG：独立列出24字节头、非零ID1与{}，不调用生产编码器。
        const std::array<unsigned char, 26> response{'S',
            'F',
            'T',
            'W',
            1,
            24,
            0x80,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            2,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            1,
            '{',
            '}'};
        bool peer_ok = true;
        std::thread peer([&] {
            UniqueFd accepted(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
            if (!accepted) {
                peer_ok = false;
                return;
            }
            timeval timeout{2, 0};
            (void)::setsockopt(accepted.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::array<char, 26> request{};
            std::size_t count = 0;
            while (count < request.size()) {
                const auto got =
                    ::recv(accepted.get(), request.data() + count, request.size() - count, 0);
                if (got <= 0)
                    return; // 失败客户端可在发完请求前关闭。
                count += static_cast<std::size_t>(got);
            }
            std::size_t sent = 0;
            while (sent < response.size()) {
                const auto n = ::send(
                    accepted.get(), response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                if (n <= 0)
                    return;
                sent += static_cast<std::size_t>(n);
            }
        });
        ClientConfig config;
        config.port = ntohs(address.sin_port);
        config.timeout_ms = 100;
        const Frame request{MessageType::ping, 0, 1, "{}"};
        mode = selected;
        sends = reads = polls = injected = 0;
        bool success = false, expected_failure = false, exception = false;
        const auto start = std::chrono::steady_clock::now();
        try {
            auto result = exchange(config, request);
            success = result && result.value().message_type == MessageType::pong
                      && result.value().body == "{}";
            expected_failure =
                !result && (selected != 5 || result.error().context == "client.timeout");
        } catch (const std::runtime_error &error) {
            exception = std::string(error.what()) == "injected unknown exception";
        }
        const int hits = injected;
        mode = 0;
        peer.join(); // 同步后读取对端观测，不以进程退出代替fd归还。
        check(peer_ok, "peer accepted");
        check(hits > 0, "fault actually injected");
        check(selected == 1   ? success
              : selected == 6 ? exception
                              : expected_failure,
            "exchange outcome");
        check(request.body == "{}" && request.request_id == 1, "request source preserved");
        if (selected == 5)
            check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
                "EINTR cannot refresh deadline");
    }
    check(descriptors() == before, "all fds returned after success/error/exception");
}
}
int main() {
    try {
        using namespace siftwing::net;
        for (int selected = 1; selected <= 6; ++selected)
            exchange_case(selected);
        ClientConfig config;
        const Frame request{MessageType::ping, 0, 1, "{}"};
        auto invalid = exchange(config, request);
        check(!invalid && invalid.error().context == "client.config", "zero port rejected");
        config.port = 1;
        auto direction = exchange(config, {MessageType::pong, 0, 1, "{}"});
        check(!direction && direction.error().context == "client.request",
            "invalid direction before socket");
        std::cout << "checks=" << checks << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
