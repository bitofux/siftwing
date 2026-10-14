/*
 * PROJECT : SIFTWING
 * FILE    : fd_handoff_queue_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 多发布者的同步可见性、FIFO与关闭提交竞争
 */
#include "siftwing/net/fd_handoff_queue.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace siftwing::net;
constexpr std::size_t producers = 4, per_producer = 64, total = producers * per_producer;
unsigned int checks = 0;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "fd handoff concurrency failed: " << label << '\n';
        std::exit(1);
    }
}
std::size_t fd_count() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++count; }
    return count;
}
struct Inputs {
    std::array<UniqueFd, total> sources, peers;
    Inputs() {
        for (std::size_t i = 0; i < total; ++i) {
            int pair[2];
            require(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, pair) == 0, "real socketpairs");
            sources[i] = UniqueFd(pair[0]); peers[i] = UniqueFd(pair[1]);
            require(::send(peers[i].get(), &i, sizeof(i), MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(i)),
                "unique payload before publish");
        }
    }
};
} // namespace
int main() {
    const auto baseline = fd_count();
    {
        Inputs inputs; FdHandoffQueue queue(7);
        // 元数据不是atomic：生产者只在提交前写，消费者只在获取该条目后读，依赖队列同步。
        std::array<std::size_t, total> published{};
        std::array<bool, total> seen{};
        std::array<std::size_t, producers> next{};
        std::atomic<bool> valid{true}, observing{true};
        std::thread observer([&] {
            while (observing.load()) {
                const auto state = queue.status();
                if (state.capacity != 7 || state.size > 7) valid.store(false);
                std::this_thread::yield();
            }
        });
        std::thread consumer([&] {
            std::size_t consumed = 0;
            while (consumed != total) {
                auto item = queue.try_pop();
                if (!item) { std::this_thread::yield(); continue; }
                std::size_t id = total;
                const auto bytes = ::recv(item->get(), &id, sizeof(id), MSG_DONTWAIT);
                if (bytes != static_cast<ssize_t>(sizeof(id)) || id >= total) { valid.store(false); return; }
                const auto producer = id / per_producer, ordinal = id % per_producer;
                if (seen[id] || ordinal != next[producer] || published[id] != id + 1000) valid.store(false);
                seen[id] = true; ++next[producer]; ++consumed;
                if (item->close()) valid.store(false);
            }
        });
        std::array<std::thread, producers> writers;
        for (std::size_t p = 0; p < producers; ++p)
            writers[p] = std::thread([&, p] {
                for (std::size_t n = 0; n < per_producer; ++n) {
                    const auto id = p * per_producer + n;
                    published[id] = id + 1000;
                    while (queue.try_push(inputs.sources[id]) != FdHandoffResult::accepted)
                        std::this_thread::yield();
                    if (inputs.sources[id]) valid.store(false);
                }
            });
        for (auto &writer : writers) writer.join();
        queue.close(); consumer.join(); observing.store(false); observer.join();
        require(valid.load(), "published data visible and per-producer FIFO");
        for (const bool value : seen) require(value, "each accepted entry consumed once");
        require(queue.status().closed && queue.status().size == 0, "closed and drained");
    }
    // 每个源独占；竞争结果用join建立可见性，目标消费者在发布者静止后清理遗留。
    for (unsigned int cycle = 0; cycle < 16; ++cycle) {
        Inputs inputs; FdHandoffQueue queue(total);
        std::array<FdHandoffResult, total> results{};
        std::atomic<bool> go{false};
        std::array<std::thread, producers> writers;
        for (std::size_t p = 0; p < producers; ++p)
            writers[p] = std::thread([&, p] {
                while (!go.load()) std::this_thread::yield();
                for (std::size_t n = 0; n < per_producer; ++n) {
                    const auto id = p * per_producer + n;
                    results[id] = queue.try_push(inputs.sources[id]);
                }
            });
        std::thread closer([&] { while (!go.load()) std::this_thread::yield(); queue.close(); });
        go.store(true);
        for (auto &writer : writers) writer.join();
        closer.join();
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < total; ++i) {
            require(results[i] == FdHandoffResult::accepted || results[i] == FdHandoffResult::closed,
                "close/push has one of two ownership outcomes");
            if (results[i] == FdHandoffResult::accepted) { ++accepted; require(!inputs.sources[i], "committed source empty"); }
            else require(static_cast<bool>(inputs.sources[i]), "rejected source retained");
        }
        std::size_t drained = 0; bool valid = true;
        std::thread target([&] {
            while (auto item = queue.try_pop()) { ++drained; if (item->close()) valid = false; }
        });
        target.join();
        require(valid && drained == accepted && queue.status().size == 0, "target drains exactly committed entries");
        require(queue.try_push(inputs.peers[0]) == FdHandoffResult::closed && inputs.peers[0],
            "post-close never accepts");
    }
    require(fd_count() == baseline, "no resource growth across concurrency and close races");
    std::cout << "fd handoff concurrency checks=" << checks << " transfers=" << total << " close_races=16\n";
}
