/*
 * PROJECT : SIFTWING
 * FILE    : multi_reactor_fixture.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 测试控制者的全Sub就绪门禁、Main停止与最后发布通知屏障
 */
#pragma once
#include "siftwing/net/protocol_sub_reactor.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace siftwing::test {
using namespace net;
/** @brief 只用于原创集成测试的外部控制者；不是正式服务API或业务实现。 */
class MultiReactorFixture final {
    std::vector<std::unique_ptr<ProtocolSubReactor>> subs_;
    std::thread main_thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool started_ = false, stopped_ = false, ready_ = false;
    MainReactor *main_borrow_ =
        nullptr; // mutex串行控制通知与所属线程撤销，不能延长fixture自身生命期。
    std::uint16_t port_ = 0;
    MainReactorStep totals_;
    std::error_code error_, ready_error_, cleanup_;
    static std::error_code exception_error() noexcept {
        try {
            throw;
        } catch (const std::system_error &error) {
            return error.code();
        } catch (const std::bad_alloc &) {
            return {ENOMEM, std::generic_category()};
        } catch (...) {
            return {EIO, std::generic_category()};
        }
    }
    void record(std::error_code error) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!error_)
            error_ = error;
    }
    void run_main(std::vector<MainReactorTarget> targets,
        ListenConfig listen,
        MainReactorConfig config) noexcept {
        std::unique_ptr<MainReactor> main;
        try {
            auto created = MainReactor::create(targets, listen, config);
            if (auto *error = std::get_if<std::error_code>(&created))
                throw std::system_error(*error);
            main = std::move(std::get<std::unique_ptr<MainReactor>>(created));
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                port_ = main->port();
                main_borrow_ = main.get();
                ready_ = true;
            }
            cv_.notify_one();
            for (;;) {
                // 固定目标任何运行首因均停止整体服务，保留Sub原始错误，不以ECANCELED遮盖它。
                for (const auto &sub : subs_)
                    if (const auto error = sub->status().first_error) {
                        record(error);
                        stop_.store(true);
                    }
                if (stop_.load()) {
                    const auto error = main->request_stop();
                    if (error)
                        record(error);
                }
                const auto step = main->run_once(100);
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    totals_.accepted += step.accepted;
                    totals_.handed_off += step.handed_off;
                    totals_.rejected += step.rejected;
                    if (!cleanup_)
                        cleanup_ = step.cleanup_error;
                }
                if (step.stop == MainReactorStop::failed || step.stop == MainReactorStop::closed) {
                    for (const auto &sub : subs_)
                        if (const auto error = sub->status().first_error)
                            record(error);
                    if (step.error)
                        record(step.error);
                    break;
                }
            }
        } catch (...) {
            const auto error = exception_error();
            record(error);
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!ready_)
                ready_error_ = error;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            main_borrow_ = nullptr;
            ready_ = true;
        }
        cv_.notify_one();
        // main在其所属线程析构；控制者join返回后才能对任何Sub作release承诺。
    }

public:
    explicit MultiReactorFixture(std::size_t count = 2, ProtocolSubConfig config = {}) {
        if (count == 0 || count > 64)
            throw std::invalid_argument("invalid fixed Sub count");
        for (std::size_t i = 0; i < count; ++i)
            subs_.push_back(std::make_unique<ProtocolSubReactor>(config));
    }
    ~MultiReactorFixture() noexcept {
        (void)stop();
    }
    MultiReactorFixture(const MultiReactorFixture &) = delete;
    MultiReactorFixture &operator=(const MultiReactorFixture &) = delete;
    std::error_code start(ListenConfig listen = {}, MainReactorConfig config = {}) {
        if (started_)
            return {EALREADY, std::generic_category()};
        started_ = true;
        try {
            for (auto &sub : subs_)
                if (const auto error = sub->start()) {
                    record(error);
                    (void)stop();
                    return error;
                }
            std::vector<MainReactorTarget> targets;
            for (auto &sub : subs_) {
                auto target = sub->target();
                if (auto *error = std::get_if<std::error_code>(&target)) {
                    record(*error);
                    (void)stop();
                    return *error;
                }
                targets.push_back(std::get<MainReactorTarget>(target));
            }
            main_thread_ = std::thread(
                &MultiReactorFixture::run_main, this, std::move(targets), listen, config);
        } catch (...) {
            const auto error = exception_error();
            record(error);
            (void)stop();
            return error;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
            return ready_;
        });
        const auto error = ready_error_;
        lock.unlock();
        if (error)
            (void)stop();
        return error;
    }
    std::error_code request_stop() {
        stop_.store(true);
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto error = main_borrow_ ? main_borrow_->request_stop() : std::error_code{};
        if (!error_)
            error_ = error;
        return error;
    }
    std::error_code stop() {
        if (!stopped_) {
            stop_main();
            // 此处Main所有发布/通知已返回；最后控制通知返回后撤raw借用，再允许目标close。
            for (auto &sub : subs_) {
                const auto error = sub->request_stop();
                if (error)
                    record(error);
            }
            for (auto &sub : subs_)
                sub->release_publishers();
            for (auto &sub : subs_)
                if (const auto error = sub->join())
                    record(error);
            stopped_ = true;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        return error_ ? error_ : cleanup_;
    }
    void stop_main() {
        (void)request_stop();
        if (main_thread_.joinable())
            main_thread_.join();
    }
    std::uint16_t port() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return port_;
    }
    MainReactorStep totals() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return totals_;
    }
    std::error_code error() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }
    ProtocolSubReactor &sub(std::size_t index) {
        return *subs_.at(index);
    }
    std::size_t size() const noexcept {
        return subs_.size();
    }
};
/** @brief Release也执行的合同检查；各测试传入自己的计数器，线程内只发布原子结果。 */
inline void require(bool value, const char *label, unsigned int &checks) {
    ++checks;
    if (!value) {
        std::cerr << "multi reactor failed: " << label << '\n';
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
