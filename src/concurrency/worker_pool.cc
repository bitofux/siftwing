/*
 * PROJECT : SIFTWING
 * FILE    : worker_pool.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 启动事务、任务异常隔离与线程回收
 */
#include "siftwing/concurrency/worker_pool.hpp"
#include <limits>
#include <stdexcept>

namespace siftwing::concurrency {
namespace {
std::size_t checked_capacity(std::size_t workers, std::size_t capacity) {
    if (workers == 0 || workers > 64)
        throw std::invalid_argument("worker count must be in [1, 64]");
    return capacity;
}
void increment(std::uint64_t &value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
} // namespace
thread_local WorkerPool *WorkerPool::current_ = nullptr;
WorkerPool::WorkerPool(std::size_t workers, std::size_t capacity)
    : queue_(checked_capacity(workers, capacity)) {
    try {
        threads_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i)
            threads_.emplace_back([this] { worker_main(); });
        {
            std::unique_lock<std::mutex> lock(start_mutex_);
            start_cv_.wait(lock, [this, workers] { return ready_ == workers; });
            start_ = true;
        }
        start_cv_.notify_all();
    } catch (...) {
        // 已创建线程仍停在门禁，没有任务可执行；先回收再传播原异常。
        rollback_start();
        throw;
    }
}
void WorkerPool::rollback_start() noexcept {
    try {
        {
            const std::lock_guard<std::mutex> lock(start_mutex_);
            abort_ = true;
            start_ = true;
        }
        start_cv_.notify_all();
        for (auto &thread : threads_)
            thread.join();
    } catch (...) {
        std::terminate();
    }
}
WorkerPool::~WorkerPool() noexcept {
    try {
        join();
    } catch (...) {
        std::terminate();
    }
}
TaskSubmitResult WorkerPool::try_submit(std::unique_ptr<WorkerTask> &source) {
    return queue_.try_submit(source);
}
void WorkerPool::request_stop() { queue_.close(); }
void WorkerPool::join() {
    // 必须在join锁前检查，避免控制者等待worker、worker反向等待join锁。
    if (current_ == this)
        throw std::logic_error("worker cannot join its own pool");
    const std::lock_guard<std::mutex> lock(join_mutex_);
    request_stop();
    for (auto &thread : threads_)
        if (thread.joinable())
            thread.join();
}
TaskQueueStatus WorkerPool::queue_status() const { return queue_.status(); }
WorkerPoolReport WorkerPool::report() const {
    const std::lock_guard<std::mutex> lock(report_mutex_);
    return report_;
}
void WorkerPool::worker_main() noexcept {
    try {
        {
            std::unique_lock<std::mutex> lock(start_mutex_);
            ++ready_;
            start_cv_.notify_all();
            start_cv_.wait(lock, [this] { return start_; });
            if (abort_)
                return;
        }
        current_ = this;
        while (auto task = queue_.wait_pop()) {
            std::exception_ptr failure;
            try {
                task->execute();
            } catch (...) {
                failure = std::current_exception();
            }
            {
                const std::lock_guard<std::mutex> lock(report_mutex_);
                if (failure) {
                    increment(report_.failed);
                    if (!report_.first_exception)
                        report_.first_exception = failure;
                } else {
                    increment(report_.succeeded);
                }
            }
            // task和failure在队列/报告锁外销毁；异常不让本worker停止消费。
        }
        current_ = nullptr;
    } catch (...) {
        // 此层只有pool同步基础设施失败；不能记录为业务异常再丢弃队列。
        std::terminate();
    }
}
} // namespace siftwing::concurrency
