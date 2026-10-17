/*
 * PROJECT : SIFTWING
 * FILE    : main_reactor_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 真实TCP/epoll目标消费者及Main停止通知失败的超时后备
 */
#include "siftwing/net/main_reactor.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
constexpr std::size_t total = 120, targets_count = 3;
unsigned int checks = 0;
thread_local bool fail_stop_write = false, watch_wait = false;
std::atomic<bool> waiting{false};
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << "Main concurrency failed: " << label << '\n';
        std::exit(1);
    }
}
std::size_t fd_count() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
} // namespace
extern "C" {
ssize_t __real_write(int, const void *, std::size_t);
ssize_t __wrap_write(int fd, const void *data, std::size_t size) {
    if (fail_stop_write) {
        errno = EIO;
        return -1;
    }
    return __real_write(fd, data, size);
}
int __real_epoll_wait(int, epoll_event *, int, int);
int __wrap_epoll_wait(int fd, epoll_event *events, int count, int timeout) {
    if (watch_wait)
        waiting.store(true);
    return __real_epoll_wait(fd, events, count, timeout);
}
}
int main() {
    const auto baseline = fd_count();
    for (unsigned int cycle = 0; cycle < 4; ++cycle) {
        FdHandoffQueue q0(total), q1(total), q2(total);
        std::array<FdHandoffQueue *, targets_count> queues{&q0, &q1, &q2};
        std::array<int, targets_count> borrowed{};
        std::array<std::atomic<bool>, targets_count> ready{};
        std::array<std::size_t, targets_count> counts{};
        std::array<std::atomic<unsigned int>, total> seen{};
        std::atomic<std::size_t> consumed{0};
        std::atomic<bool> quiescent{false}, stop_returned{false}, valid{true};
        std::array<std::thread, targets_count> consumers;
        for (std::size_t target = 0; target < targets_count; ++target)
            consumers[target] = std::thread([&, target] {
                auto created = create_eventfd_wakeup();
                auto poller = create_epoll();
                if (!std::holds_alternative<UniqueFd>(created)
                    || !std::holds_alternative<UniqueFd>(poller))
                    std::exit(2);
                auto fd = std::move(std::get<UniqueFd>(created)),
                     epoll = std::move(std::get<UniqueFd>(poller));
                if (epoll_add(epoll.get(), fd.get(), EPOLLIN, 7))
                    std::exit(2);
                borrowed[target] = fd.get();
                ready[target].store(true);
                for (;;) {
                    epoll_event event{};
                    const auto count = ::epoll_wait(
                        epoll.get(), &event, 1, queues[target]->status().size ? 0 : 10);
                    if (count < 0 && errno != EINTR)
                        std::exit(2);
                    if (drain_eventfd(fd.get()).state == EventFdDrainState::failed)
                        std::exit(2);
                    for (unsigned int n = 0; n < 4; ++n) {
                        auto item = queues[target]->try_pop();
                        if (!item)
                            break;
                        // 真实TCP允许短读：独立消费者拼成8字节id；Main完全不读这些字节。
                        std::uint64_t id = total;
                        std::size_t offset = 0;
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        while (offset != sizeof(id)) {
                            const auto bytes = ::recv(item->get(),
                                reinterpret_cast<char *>(&id) + offset,
                                sizeof(id) - offset,
                                MSG_DONTWAIT);
                            if (bytes > 0)
                                offset += static_cast<std::size_t>(bytes);
                            else if (bytes < 0 && (errno == EAGAIN || errno == EINTR))
                                std::this_thread::yield();
                            else
                                std::exit(2);
                            if (std::chrono::steady_clock::now() >= deadline)
                                std::exit(2);
                        }
                        if (id >= total || seen[static_cast<std::size_t>(id)].fetch_add(1) != 0)
                            valid.store(false);
                        if (item->close())
                            valid.store(false);
                        ++counts[target];
                        consumed.fetch_add(1);
                    }
                    if (quiescent.load() && queues[target]->status().size == 0)
                        break;
                }
                if (epoll_remove(epoll.get(), fd.get()) || fd.close() || epoll.close())
                    valid.store(false);
            });
        for (const auto &flag : ready)
            while (!flag.load())
                std::this_thread::yield();
        std::atomic<MainReactor *> main_borrow{nullptr};
        std::atomic<std::uint16_t> port{0};
        std::size_t accepted = 0, handed_off = 0;
        std::thread main_thread([&] {
            MainReactorConfig config;
            config.accept_attempt_budget = 8;
            auto created = MainReactor::create(
                {{&q0, borrowed[0]}, {&q1, borrowed[1]}, {&q2, borrowed[2]}}, {}, config);
            if (!std::holds_alternative<std::unique_ptr<MainReactor>>(created))
                std::exit(2);
            auto main = std::move(std::get<std::unique_ptr<MainReactor>>(created));
            port.store(main->port());
            main_borrow.store(main.get());
            for (;;) {
                const auto result = main->run_once(100);
                accepted += result.accepted;
                handed_off += result.handed_off;
                if (result.rejected || result.error || result.cleanup_error
                    || result.accept_attempts > 8)
                    valid.store(false);
                if (result.stop == MainReactorStop::closed)
                    break;
                if (result.stop == MainReactorStop::failed)
                    std::exit(2);
            }
            // 跨线程停止入口返回前对象仍存活，不能从run已退出推断借用者已返回。
            while (!stop_returned.load())
                std::this_thread::yield();
            main_borrow.store(nullptr);
        });
        while (!main_borrow.load())
            std::this_thread::yield();
        std::array<std::thread, 2> clients;
        for (std::size_t writer = 0; writer < clients.size(); ++writer)
            clients[writer] = std::thread([&, writer] {
                for (std::size_t ordinal = 0; ordinal < total / clients.size(); ++ordinal) {
                    UniqueFd peer(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
                    if (!peer)
                        std::exit(2);
                    sockaddr_in address{};
                    address.sin_family = AF_INET;
                    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    address.sin_port = htons(port.load());
                    if (::connect(
                            peer.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)))
                        std::exit(2);
                    const std::uint64_t id = writer * (total / clients.size()) + ordinal;
                    if (::send(peer.get(), &id, sizeof(id), MSG_NOSIGNAL)
                        != static_cast<ssize_t>(sizeof(id)))
                        std::exit(2);
                }
            });
        for (auto &writer : clients)
            writer.join();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (consumed.load() != total) {
            if (std::chrono::steady_clock::now() >= deadline)
                std::exit(2);
            std::this_thread::yield();
        }
        require(!main_borrow.load()->request_stop(), "cross-thread real eventfd stop");
        stop_returned.store(true);
        main_thread.join();
        for (const auto fd : borrowed)
            require(!notify_eventfd(fd), "last target control notifications return");
        quiescent.store(true);
        for (auto &consumer : consumers)
            consumer.join();
        require(valid.load() && accepted == total && handed_off == total,
            "Main only accepts and hands off");
        for (const auto count : counts)
            require(count == total / targets_count, "independent round robin count");
        for (const auto &count : seen)
            require(count.load() == 1, "exactly one target consumes each TCP id");
    }
    for (const bool inject_failure : {false, true}) {
        // 空Main无旧连接通知；包装只观察真实wait，失败注入仅控制线程TLS，不改目标消费者。
        FdHandoffQueue queue(1);
        auto created = create_eventfd_wakeup();
        require(std::holds_alternative<UniqueFd>(created), "fallback target");
        auto target = std::move(std::get<UniqueFd>(created));
        std::atomic<MainReactor *> main_borrow{nullptr};
        std::atomic<bool> returned{false};
        waiting.store(false);
        bool stopped = false;
        std::thread main_thread([&] {
            auto result = MainReactor::create({{&queue, target.get()}});
            if (!std::holds_alternative<std::unique_ptr<MainReactor>>(result))
                std::exit(2);
            auto main = std::move(std::get<std::unique_ptr<MainReactor>>(result));
            main_borrow.store(main.get());
            watch_wait = true;
            const auto step = main->run_once(100);
            watch_wait = false;
            stopped = step.stop == MainReactorStop::closed && !step.error && !step.cleanup_error;
            while (!returned.load())
                std::this_thread::yield();
            main_borrow.store(nullptr);
        });
        while (!waiting.load())
            std::this_thread::yield();
        fail_stop_write = inject_failure;
        const auto error = main_borrow.load()->request_stop();
        fail_stop_write = false;
        require(error.value() == (inject_failure ? EIO : 0),
            "stop write diagnostic does not revoke stop flag");
        returned.store(true);
        main_thread.join();
        require(stopped && queue.status().closed, "real finite wait or wakeup reaches stopped");
    }
    require(fd_count() == baseline, "no descriptor growth");
    std::cout << "Main concurrency checks=" << checks << '\n';
}
