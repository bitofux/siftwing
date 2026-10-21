/*
 * PROJECT : SIFTWING
 * FILE    : net_test_support.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-21
 * BRIEF   : 原创网络黄金字节、真实socket与有限等待辅助，不装配网络线程
 */
#pragma once
#include "siftwing/net/listener.hpp"
#include "siftwing/net/protocol_connection_config.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
namespace siftwing::test {
using namespace net;
inline thread_local void (*io_progress)() = nullptr;
/** @brief Release也执行的合同检查；各测试传入自己的计数器，线程内只发布原子结果。 */
inline void require(bool value, const char *label, unsigned int &checks) {
    ++checks;
    if (!value) {
        std::cerr << "network contract failed: " << label << '\n';
        std::exit(1);
    }
}
/** @brief 原创24字节黄金编码，不调用生产codec生成期望响应。 */
inline std::string wire(
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
inline UniqueFd client(std::uint16_t port, IpFamily family = IpFamily::v4) {
    UniqueFd fd(
        ::socket(family == IpFamily::v4 ? AF_INET : AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd)
        throw std::system_error(errno, std::generic_category());
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(port);
    v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port);
    v6.sin6_addr = in6addr_loopback;
    const auto *address = family == IpFamily::v4 ? reinterpret_cast<const sockaddr *>(&v4)
                                                 : reinterpret_cast<const sockaddr *>(&v6);
    if (::connect(fd.get(),
            address,
            static_cast<socklen_t>(family == IpFamily::v4 ? sizeof(v4) : sizeof(v6))))
        throw std::system_error(errno, std::generic_category());
    return fd;
}
inline void send_all(int fd, std::string_view bytes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bytes.empty()) {
        if (io_progress)
            io_progress();
        const auto n = ::send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0)
            bytes.remove_prefix(static_cast<std::size_t>(n));
        else if (n < 0 && (errno == EAGAIN || errno == EINTR))
            std::this_thread::yield();
        else
            throw std::system_error(n == 0 ? EIO : errno, std::generic_category());
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("send deadline");
    }
}
inline std::string receive(int fd, std::size_t size) {
    std::string bytes;
    std::array<char, 512> block{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bytes.size() < size) {
        if (io_progress)
            io_progress();
        const auto n =
            ::recv(fd, block.data(), std::min(block.size(), size - bytes.size()), MSG_DONTWAIT);
        if (n > 0)
            bytes.append(block.data(), static_cast<std::size_t>(n));
        else if (n < 0 && (errno == EAGAIN || errno == EINTR))
            std::this_thread::yield();
        else
            throw std::runtime_error("unexpected EOF/recv failure");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("receive deadline");
    }
    return bytes;
}
template <class Predicate>
void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (io_progress)
            io_progress();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("state deadline");
        std::this_thread::yield();
    }
}
inline std::size_t fd_count() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
} // namespace siftwing::test
