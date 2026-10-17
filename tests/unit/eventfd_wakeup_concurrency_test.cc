/*
 * PROJECT : SIFTWING
 * FILE    : eventfd_wakeup_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 真实epoll消费者、多个发布者、有限批次及通知借用静止门禁
 */
#include "siftwing/net/eventfd_wakeup.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
constexpr std::size_t producers = 4, each = 64, total = producers * each;
unsigned int checks = 0;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "eventfd concurrency failed: " << label << '\n';
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
int main() {
    const auto baseline = fd_count();
    for (unsigned int cycle = 0; cycle < 12; ++cycle) {
        FdHandoffQueue queue(7);
        std::array<UniqueFd, total> sources, peers;
        for (std::size_t id = 0; id < total; ++id) {
            int pair[2];
            require(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, pair) == 0, "real sockets");
            sources[id] = UniqueFd(pair[0]);
            peers[id] = UniqueFd(pair[1]);
            require(::send(peers[id].get(), &id, sizeof(id), MSG_NOSIGNAL)
                        == static_cast<ssize_t>(sizeof(id)),
                "payload");
        }
        std::array<std::size_t, total> published{};
        std::array<bool, total> seen{};
        std::array<std::size_t, producers> next{};
        std::atomic<bool> ready{false}, quiescent{false}, valid{true};
        int borrowed = -1;
        std::size_t consumed = 0, wakes = 0, batches = 0;
        std::thread consumer([&] {
            auto result = create_eventfd_wakeup();
            auto epoll_result = create_epoll();
            if (!std::holds_alternative<UniqueFd>(result)
                || !std::holds_alternative<UniqueFd>(epoll_result))
                std::exit(2);
            auto fd = std::move(std::get<UniqueFd>(result));
            auto epoll = std::move(std::get<UniqueFd>(epoll_result));
            if (epoll_add(epoll.get(), fd.get(), EPOLLIN, 77))
                std::exit(2);
            borrowed = fd.get();
            ready.store(true); // release/acquire发布稳定借用，关闭前另有quiescent门禁。
            for (;;) {
                epoll_event event{};
                // 剩余队列不空时不阻塞；有限等待也让停止通知失败后仍能检查门禁。
                const int count =
                    ::epoll_wait(epoll.get(), &event, 1, queue.status().size ? 0 : 20);
                if (count < 0 && errno != EINTR) {
                    valid.store(false);
                    break;
                }
                if (count > 0) {
                    ++wakes;
                    if (event.data.u64 != 77 || !(event.events & EPOLLIN))
                        valid.store(false);
                }
                if (drain_eventfd(fd.get(), 4).state == EventFdDrainState::failed)
                    valid.store(false);
                ++batches;
                for (std::size_t n = 0; n < 3; ++n) {
                    auto item = queue.try_pop();
                    if (!item)
                        break;
                    std::size_t id = total;
                    if (::recv(item->get(), &id, sizeof(id), MSG_DONTWAIT)
                            != static_cast<ssize_t>(sizeof(id))
                        || id >= total) {
                        valid.store(false);
                        continue;
                    }
                    const auto p = id / each;
                    if (seen[id] || id % each != next[p] || published[id] != id + 1000)
                        valid.store(false);
                    seen[id] = true;
                    ++next[p];
                    ++consumed;
                    if (item->close())
                        valid.store(false);
                }
                // closed+empty不证明生产者静止；只能在最后通知也返回的门禁之后关闭eventfd。
                if (quiescent.load() && queue.status().size == 0)
                    break;
            }
            if (epoll_remove(epoll.get(), fd.get()) || fd.close() || epoll.close())
                valid.store(false);
        });
        while (!ready.load())
            std::this_thread::yield();
        std::array<std::thread, producers> writers;
        for (std::size_t p = 0; p < producers; ++p)
            writers[p] = std::thread([&, p] {
                for (std::size_t n = 0; n < each; ++n) {
                    const auto id = p * each + n;
                    published[id] = id + 1000;
                    for (;;) {
                        const auto result = try_handoff_and_notify(queue, sources[id], borrowed);
                        if (result.handoff == FdHandoffResult::accepted) {
                            if (sources[id] || result.notification_error)
                                valid.store(false);
                            break;
                        }
                        if (result.handoff != FdHandoffResult::full || !sources[id])
                            valid.store(false);
                        std::this_thread::yield();
                    }
                }
            });
        for (auto &writer : writers)
            writer.join();
        queue.close();
        if (cycle % 2 == 0)
            require(!notify_eventfd(borrowed), "last control notification returns");
        else
            require(notify_eventfd(-1).value() == EBADF,
                "failed stop notification needs finite wait fallback");
        quiescent.store(true);
        consumer.join();
        require(valid.load() && consumed == total && wakes > 0 && batches > 0,
            "real wakes and bounded consumer");
        for (const bool value : seen)
            require(value, "each accepted fd exactly once");
        require(queue.status().closed && queue.status().size == 0,
            "closed and drained after publishers return");
    }
    {
        // 独立空队列场景：没有任何旧通知可帮忙，必须由真实有限epoll超时检查停止标志。
        std::atomic<bool> ready{false}, stop{false};
        bool fallback = false;
        std::thread consumer([&] {
            auto created = create_eventfd_wakeup();
            auto epoll_created = create_epoll();
            if (!std::holds_alternative<UniqueFd>(created)
                || !std::holds_alternative<UniqueFd>(epoll_created))
                std::exit(2);
            auto fd = std::move(std::get<UniqueFd>(created));
            auto epoll = std::move(std::get<UniqueFd>(epoll_created));
            if (epoll_add(epoll.get(), fd.get(), EPOLLIN, 88))
                std::exit(2);
            ready.store(true);
            do {
                epoll_event event{};
                const int count = ::epoll_wait(epoll.get(), &event, 1, 10);
                if (count != 0)
                    std::exit(2);
                fallback = true;
            } while (!stop.load());
            if (epoll_remove(epoll.get(), fd.get()) || fd.close() || epoll.close())
                std::exit(2);
        });
        while (!ready.load())
            std::this_thread::yield();
        require(notify_eventfd(-1).value() == EBADF,
            "no valid stop notification reaches empty consumer");
        stop.store(true);
        consumer.join();
        require(fallback, "actual timeout reaches stop without an event");
    }
    require(fd_count() == baseline, "no fd growth");
    std::cout << "eventfd concurrency checks=" << checks << '\n';
}
