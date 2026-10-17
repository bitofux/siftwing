/*
 * PROJECT : SIFTWING
 * FILE    : worker_pool_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 单worker顺序、容量、任务异常与销毁归属合同
 */
#include "siftwing/concurrency/worker_pool.hpp"
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace siftwing::concurrency;
unsigned int checks = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void hold() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] {
            return released;
        });
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock,
                    std::chrono::seconds(5),
                    [this] {
                        return entered;
                    }),
            "worker entered");
    }
    void release() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        cv.notify_all();
    }
};
struct Job final : WorkerTask {
    std::function<void()> body, cleanup;
    Job(std::function<void()> action, std::function<void()> destroy = {})
        : body(std::move(action)), cleanup(std::move(destroy)) {}
    void execute() override {
        body();
    }
    ~Job() noexcept override {
        if (cleanup)
            cleanup();
    }
};
std::unique_ptr<WorkerTask> job(std::function<void()> body, std::function<void()> cleanup = {}) {
    return std::make_unique<Job>(std::move(body), std::move(cleanup));
}
}
int main() {
    for (const auto workers : {std::size_t(0), std::size_t(65)}) {
        bool caught = false;
        try {
            WorkerPool pool(workers, 1);
        } catch (const std::invalid_argument &) {
            caught = true;
        }
        require(caught, "invalid worker count");
    }
    bool caught = false;
    try {
        WorkerPool pool(1, 0);
    } catch (const std::invalid_argument &) {
        caught = true;
    }
    require(caught, "invalid capacity");
    Gate gate;
    WorkerPool pool(1, 3);
    auto blocker = job([&] {
        gate.hold();
    });
    require(
        pool.try_submit(blocker) == TaskSubmitResult::accepted && !blocker, "accepted consumes");
    gate.wait();
    std::vector<int> order;
    std::thread::id executor, destroyer;
    unsigned int destroyed = 0;
    // 一worker被屏障占住后填满三个槽；队列容量不含正在执行的blocker。
    auto first = job(
        [&] {
            order.push_back(1);
            throw 17;
        },
        [&] {
            ++destroyed;
        });
    auto second = job(
        [&] {
            order.push_back(2);
            throw std::runtime_error("second");
        },
        [&] {
            ++destroyed;
        });
    auto third = job(
        [&] {
            order.push_back(3);
            executor = std::this_thread::get_id();
        },
        [&] {
            destroyer = std::this_thread::get_id();
            (void)pool.queue_status();
            (void)pool.report(); // 重入证明队列/报告锁外析构。
            ++destroyed;
        });
    require(pool.try_submit(first) == TaskSubmitResult::accepted, "first accepted");
    require(pool.try_submit(second) == TaskSubmitResult::accepted, "second accepted");
    require(pool.try_submit(third) == TaskSubmitResult::accepted, "third accepted");
    auto rejected = job([] {
    });
    const auto *identity = rejected.get();
    require(pool.try_submit(rejected) == TaskSubmitResult::full && rejected.get() == identity,
        "full retains");
    std::unique_ptr<WorkerTask> empty;
    require(pool.try_submit(empty) == TaskSubmitResult::invalid_task, "null precedes full");
    require(pool.queue_status().size == 3 && !pool.queue_status().closed, "bounded pending");
    pool.request_stop();
    pool.request_stop();
    require(pool.try_submit(empty) == TaskSubmitResult::closed, "closed precedes null");
    require(pool.try_submit(rejected) == TaskSubmitResult::closed && rejected.get() == identity,
        "closed retains");
    gate.release();
    pool.join();
    pool.join();
    require(order == std::vector<int>({1, 2, 3}), "single worker FIFO and continues after failure");
    require(destroyed == 3 && executor == destroyer && executor != std::this_thread::get_id(),
        "worker destroys");
    const auto report = pool.report();
    require(report.succeeded == 2 && report.failed == 2 && report.first_exception,
        "exact final report");
    caught = false;
    try {
        std::rethrow_exception(report.first_exception);
    } catch (int value) {
        caught = value == 17;
    }
    require(caught, "first nonstandard exception retained");
    require(pool.queue_status().closed && pool.queue_status().size == 0, "closed drained");
    // 本池worker在控制者可能正join时仍须立即拒绝self-join，不能反向等join锁。
    bool self_rejected = false, remained_open = false;
    WorkerPool self(1, 1);
    Gate self_gate;
    auto self_task = job([&] {
        self_gate.hold();
        try {
            self.join();
        } catch (const std::logic_error &) {
            self_rejected = true;
        }
        remained_open = !self.queue_status().closed;
    });
    require(self.try_submit(self_task) == TaskSubmitResult::accepted, "self task accepted");
    self_gate.wait();
    self_gate.release();
    // 用第二任务的执行同步检查；尚不在控制者close，避免把closed观测变成竞态。
    auto after_self = job([] {
    });
    while (self.try_submit(after_self) == TaskSubmitResult::full)
        std::this_thread::yield();
    while (self.report().succeeded != 2)
        std::this_thread::yield();
    self.join();
    require(self_rejected && remained_open && self.report().failed == 0,
        "self join rejected without close");
    Gate join_gate;
    bool blocked_self_rejected = false;
    WorkerPool joining(1, 1);
    auto join_task = job([&] {
        join_gate.hold();
        try {
            joining.join();
        } catch (const std::logic_error &) {
            blocked_self_rejected = true;
        }
    });
    require(joining.try_submit(join_task) == TaskSubmitResult::accepted, "join race task accepted");
    join_gate.wait();
    std::thread controller([&] {
        joining.join();
    });
    while (!joining.queue_status().closed)
        std::this_thread::yield();
    join_gate.release();
    controller.join();
    require(blocked_self_rejected, "self join rejected before controller lock");
    // worker自己关闭入口仍排空后续条目，抛出的self-join异常由任务边界隔离。
    Gate stop_gate;
    WorkerPool stopping(1, 2);
    auto stop_task = job([&] {
        stop_gate.hold();
        stopping.request_stop();
        stopping.join();
    });
    require(stopping.try_submit(stop_task) == TaskSubmitResult::accepted, "worker stop accepted");
    stop_gate.wait();
    auto tail = job([] {
    });
    require(stopping.try_submit(tail) == TaskSubmitResult::accepted,
        "tail accepted before worker stop");
    stop_gate.release();
    stopping.join();
    require(stopping.report().failed == 1 && stopping.report().succeeded == 1,
        "worker stop and uncaught self join isolate then drain");
    unsigned int auto_drained = 0;
    {
        WorkerPool automatic(2, 8);
        for (unsigned int i = 0; i < 8; ++i) {
            // 多worker共享结果必须独立同步，不能用普通自增证明线程安全。
            auto task = job(
                [] {
                },
                [&] {
                    const std::lock_guard<std::mutex> lock(gate.mutex);
                    ++auto_drained;
                });
            require(automatic.try_submit(task) == TaskSubmitResult::accepted, "automatic accepted");
        }
    }
    require(auto_drained == 8, "destructor drains and joins");
    {
        WorkerPool idle(4, 1);
        idle.join();
        require(idle.report().succeeded == 0, "idle wake and join");
    }
    std::cout << "worker pool checks=" << checks << '\n';
}
