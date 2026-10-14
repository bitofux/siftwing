/*
 * PROJECT : SIFTWING
 * FILE    : worker_pool_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 多生产者执行可见性、精确一次及并发停止join
 */
#include "siftwing/concurrency/worker_pool.hpp"
#include <atomic>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
using namespace siftwing::concurrency;
unsigned int checks = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) { std::cerr << label << '\n'; std::exit(1); }
}
constexpr std::size_t total = 1024;
struct Observation {
    std::array<std::atomic<unsigned int>, total> calls;
    std::atomic<unsigned int> destroyed{0}, bad{0};
    Observation() { for (auto &value : calls) std::atomic_init(&value, 0U); }
};
struct Job final : WorkerTask {
    Observation &observed;
    std::size_t id;
    std::size_t payload; // 提交前写入，非原子字段由队列锁发布。
    std::thread::id executor;
    Job(Observation &values, std::size_t index) : observed(values), id(index), payload(index * 13 + 7) {}
    void execute() override {
        executor = std::this_thread::get_id();
        if (payload != id * 13 + 7) ++observed.bad;
        ++observed.calls[id];
        if (id % 7 == 0) throw std::runtime_error("task");
    }
    ~Job() noexcept override {
        if (executor != std::thread::id{} && executor != std::this_thread::get_id()) ++observed.bad;
        ++observed.destroyed;
    }
};
}
int main() {
    for (unsigned int round = 0; round < 6; ++round) {
        Observation observed;
        WorkerPool pool(3, 7);
        std::array<std::thread, 4> producers;
        for (std::size_t p = 0; p < producers.size(); ++p) producers[p] = std::thread([&, p] {
            for (std::size_t i = p * 256; i < (p + 1) * 256; ++i) {
                std::unique_ptr<WorkerTask> task = std::make_unique<Job>(observed, i);
                auto result = pool.try_submit(task);
                while (result == TaskSubmitResult::full) {
                    std::this_thread::yield(); result = pool.try_submit(task);
                }
                if (result != TaskSubmitResult::accepted || task) ++observed.bad;
            }
        });
        for (auto &producer : producers) producer.join();
        // 多控制者请求关闭与join同一pool；所有任务已提交，必须继续排空。
        std::thread a([&] { pool.join(); }), b([&] { pool.join(); }), c([&] { pool.request_stop(); });
        a.join(); b.join(); c.join();
        unsigned int failures = 0;
        for (std::size_t i = 0; i < total; ++i) {
            require(observed.calls[i] == 1, "MPMC exactly once");
            if (i % 7 == 0) ++failures;
        }
        const auto report = pool.report();
        require(report.failed == failures && report.succeeded == total - failures, "MPMC exception counts");
        require(observed.bad == 0 && observed.destroyed == total, "published payload and worker destruction");
    }
    for (unsigned int round = 0; round < 32; ++round) {
        Observation observed;
        WorkerPool pool(4, 3);
        std::array<std::atomic<bool>, total> accepted;
        for (auto &value : accepted) std::atomic_init(&value, false);
        std::atomic<bool> go{false};
        std::array<std::thread, 8> producers;
        for (std::size_t p = 0; p < producers.size(); ++p) producers[p] = std::thread([&, p] {
            while (!go.load()) std::this_thread::yield();
            for (std::size_t i = p * 128; i < (p + 1) * 128; ++i) {
                std::unique_ptr<WorkerTask> task = std::make_unique<Job>(observed, i);
                const auto *identity = task.get();
                const auto result = pool.try_submit(task);
                if (result == TaskSubmitResult::accepted) {
                    accepted[i] = true;
                    if (task) ++observed.bad;
                } else if (task.get() != identity || (result != TaskSubmitResult::closed && result != TaskSubmitResult::full)) {
                    ++observed.bad;
                }
            }
        });
        std::thread stopper([&] { while (!go.load()) std::this_thread::yield(); pool.join(); });
        go = true;
        // join可能先返回；this保持到所有生产者完整调用返回，不能把join当发布者屏障。
        stopper.join();
        for (auto &producer : producers) producer.join();
        pool.join();
        std::uint64_t succeeded = 0, failed = 0;
        for (std::size_t i = 0; i < total; ++i) {
            require(observed.calls[i] == (accepted[i] ? 1U : 0U), "submit-close commit ownership");
            if (accepted[i]) { if (i % 7 == 0) ++failed; else ++succeeded; }
        }
        const auto report = pool.report();
        require(report.succeeded == succeeded && report.failed == failed, "race final report");
        require(observed.bad == 0 && observed.destroyed == total, "race destroys accepted and rejected once");
    }
    std::cout << "worker concurrency checks=" << checks << '\n';
}
