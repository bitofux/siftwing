/*
 * PROJECT : SIFTWING
 * FILE    : task_queue_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 真实阻塞唤醒、MPMC精确消费与关闭提交竞争
 */
#include "siftwing/concurrency/task_queue.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace siftwing::concurrency;
unsigned int checks = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << "task concurrency failed: " << label << '\n';
        std::exit(1);
    }
}
template <class Predicate>
void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            require(false, "observation deadline");
        std::this_thread::yield();
    }
    require(true, "predicate observed before deadline");
}
struct Job {
    std::size_t id, payload;
};
using Queue = TaskQueue<Job>;
bool parked(long tid) {
    if (!tid)
        return false;
    std::ifstream input("/proc/self/task/" + std::to_string(tid) + "/wchan");
    std::string state;
    input >> state;
    return state.find("futex") != std::string::npos;
}
constexpr std::size_t producers = 4, per_producer = 128, total = producers * per_producer;
} // namespace
int main() {
    // 线程唯一阻塞入口是wait_pop；观察Linux futex停车后才提交/close，不靠固定sleep猜测。
    for (unsigned int round = 0; round < 8; ++round) {
        Queue queue(1);
        constexpr std::size_t count = 8;
        std::array<std::atomic<long>, count> tids{};
        std::array<std::atomic<bool>, count> done{};
        // C++17下在线程发布前显式初始化每个atomic对象。
        for (auto &tid : tids)
            std::atomic_init(&tid, 0L);
        for (auto &finished : done)
            std::atomic_init(&finished, false);
        std::array<std::thread, count> readers;
        std::atomic<unsigned int> received{0}, empty{0};
        std::atomic<bool> valid{true};
        for (std::size_t i = 0; i < count; ++i)
            readers[i] = std::thread([&, i] {
                tids[i] = ::syscall(SYS_gettid);
                auto task = queue.wait_pop();
                if (task) {
                    if (task->id != 7 || task->payload != 1007)
                        valid = false;
                    received.fetch_add(1);
                } else
                    empty.fetch_add(1);
                done[i] = true;
            });
        await([&] {
            for (std::size_t i = 0; i < count; ++i)
                if (!parked(tids[i].load()))
                    return false;
            return true;
        });
        require(received == 0 && empty == 0, "all open empty consumers blocked");
        auto task = std::make_unique<Job>(Job{7, 1007});
        require(
            queue.try_submit(task) == TaskSubmitResult::accepted && !task, "submit wakes consumer");
        await([&] {
            return received.load() == 1;
        });
        await([&] {
            for (std::size_t i = 0; i < count; ++i)
                if (!done[i].load() && !parked(tids[i].load()))
                    return false;
            return true;
        });
        require(empty == 0 && !queue.status().closed, "others remain waiting for predicate");
        queue.close();
        for (auto &reader : readers)
            reader.join();
        require(valid && received == 1 && empty == count - 1, "close wakes every empty consumer");
        require(!queue.wait_pop(), "repeated closed wait returns immediately");
    }
    // 已发布payload不是atomic；只通过queue锁交接读写权限，TSan检查此可见性。
    for (unsigned int round = 0; round < 8; ++round) {
        Queue queue(7);
        std::array<std::atomic<unsigned int>, total> seen{};
        for (auto &value : seen)
            std::atomic_init(&value, 0U);
        std::atomic<bool> valid{true}, observing{true};
        std::thread observer([&] {
            while (observing.load()) {
                const auto status = queue.status();
                if (status.capacity != 7 || status.size > 7)
                    valid = false;
                std::this_thread::yield();
            }
        });
        std::array<std::thread, 3> readers;
        for (auto &reader : readers)
            reader = std::thread([&] {
                while (auto task = queue.wait_pop()) {
                    if (task->id >= total || task->payload != task->id + 1000) {
                        valid = false;
                        continue;
                    }
                    if (seen[task->id].fetch_add(1) != 0)
                        valid = false;
                }
            });
        std::array<std::thread, producers> writers;
        for (std::size_t p = 0; p < producers; ++p)
            writers[p] = std::thread([&, p] {
                for (std::size_t n = 0; n < per_producer; ++n) {
                    const auto id = p * per_producer + n;
                    auto task = std::make_unique<Job>();
                    task->id = id;
                    task->payload = id + 1000;
                    for (;;) {
                        const auto result = queue.try_submit(task);
                        if (result == TaskSubmitResult::accepted)
                            break;
                        if (result != TaskSubmitResult::full || !task) {
                            valid = false;
                            return;
                        }
                        std::this_thread::yield();
                    }
                    if (task)
                        valid = false;
                }
            });
        for (auto &writer : writers)
            writer.join();
        queue.close();
        for (auto &reader : readers)
            reader.join();
        observing = false;
        observer.join();
        require(valid && queue.status().size == 0, "MPMC valid publication and bounded drain");
        for (const auto &occurrences : seen)
            require(occurrences == 1, "each committed task exactly once");
    }
    {
        Queue queue(3);
        std::array<std::size_t, producers> next{};
        std::atomic<bool> valid{true};
        // 单消费者可独立观察每个生产者的提交顺序；不以多消费者完成次序冒称FIFO。
        std::thread reader([&] {
            while (auto task = queue.wait_pop()) {
                const auto p = task->id / per_producer, ordinal = task->id % per_producer;
                if (p >= producers || ordinal != next[p]++)
                    valid = false;
            }
        });
        std::array<std::thread, producers> writers;
        for (std::size_t p = 0; p < producers; ++p)
            writers[p] = std::thread([&, p] {
                for (std::size_t n = 0; n < per_producer; ++n) {
                    auto task = std::make_unique<Job>(Job{p * per_producer + n, 0});
                    while (queue.try_submit(task) != TaskSubmitResult::accepted)
                        std::this_thread::yield();
                }
            });
        for (auto &writer : writers)
            writer.join();
        queue.close();
        reader.join();
        require(valid, "per producer FIFO");
        for (const auto count : next)
            require(count == per_producer, "FIFO drains all producers");
    }
    for (unsigned int round = 0; round < 64; ++round) {
        Queue queue(7);
        constexpr std::size_t count = 16;
        std::array<std::unique_ptr<Job>, count> sources;
        std::array<TaskSubmitResult, count> outcomes{};
        std::array<unsigned int, count> seen{};
        std::array<Job *, count> identities{};
        for (std::size_t i = 0; i < count; ++i) {
            sources[i] = std::make_unique<Job>(Job{i, i + 1000});
            identities[i] = sources[i].get();
        }
        std::atomic<bool> begin{false}, valid{true};
        std::array<std::thread, count> writers;
        std::thread reader([&] {
            while (auto task = queue.wait_pop()) {
                if (task->id >= count || task->payload != task->id + 1000)
                    valid = false;
                else
                    ++seen[task->id];
            }
        });
        for (std::size_t i = 0; i < count; ++i)
            writers[i] = std::thread([&, i] {
                while (!begin.load())
                    std::this_thread::yield();
                outcomes[i] = queue.try_submit(sources[i]);
            });
        std::thread closer([&] {
            while (!begin.load())
                std::this_thread::yield();
            queue.close();
        });
        begin = true;
        for (auto &writer : writers)
            writer.join();
        closer.join();
        reader.join();
        require(
            valid && queue.status().closed && queue.status().size == 0, "close/commit race drains");
        for (std::size_t i = 0; i < count; ++i) {
            if (outcomes[i] == TaskSubmitResult::accepted)
                require(!sources[i] && seen[i] == 1, "accepted owned only by consumer");
            else
                require((outcomes[i] == TaskSubmitResult::closed
                            || outcomes[i] == TaskSubmitResult::full)
                            && sources[i].get() == identities[i] && seen[i] == 0,
                    "rejection retains exact source");
        }
    }
    std::cout << "task concurrency checks=" << checks << '\n';
}
