/*
 * PROJECT : SIFTWING
 * FILE    : worker_pool_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 启动分配故障回滚、初次锁错误与无遗留线程
 */
#include "siftwing/concurrency/worker_pool.hpp"
#include <array>
#include <chrono>
#include <set>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <pthread.h>

namespace {
using namespace siftwing::concurrency;
thread_local unsigned int fail_at = 0, allocations = 0;
thread_local bool fail_lock = false, observing = false;
unsigned int checks = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}
std::set<std::string> task_ids() {
    std::set<std::string> ids;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task"))
        ids.insert(entry.path().filename().string());
    return ids;
}
template <class Predicate>
bool observed_within(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (predicate())
            return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
struct WarmState {
    std::mutex mutex;
    std::condition_variable cv;
    std::array<long, 4> tids{};
    std::size_t ready = 0;
    bool release = false;
};
struct WarmJob final : WorkerTask {
    WarmState &state;
    explicit WarmJob(WarmState &shared) : state(shared) {}
    void execute() override {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.tids[state.ready++] = ::syscall(SYS_gettid);
        state.cv.notify_all();
        state.cv.wait(lock, [this] {
            return state.release;
        });
    }
};
struct Job final : WorkerTask {
    void execute() override {}
};
template <class Action>
void lock_failure(Action action) {
    fail_lock = true;
    bool caught = false;
    try {
        action();
    } catch (const std::system_error &error) {
        caught = error.code().value() == EINVAL;
    }
    require(caught && !fail_lock, "initial lock error propagated");
}
}
extern "C" {
void *__real__Znwm(std::size_t);
void *__wrap__Znwm(std::size_t size) {
    if (observing) {
        ++allocations;
        if (fail_at != 0 && allocations == fail_at)
            throw std::bad_alloc();
    }
    return __real__Znwm(size);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (fail_lock) {
        fail_lock = false;
        return EINVAL;
    }
    return __real_pthread_mutex_lock(mutex);
}
}
int main() {
    WarmState warm;
    allocations = 0;
    observing = true;
    WorkerPool warmed(4, 4);
    observing = false;
    const auto allocation_count = allocations;
    // 先让四个worker各执行一个阻塞任务并记录tid；TSan辅助线程在此时已经建立。
    for (std::size_t i = 0; i < warm.tids.size(); ++i) {
        std::unique_ptr<WorkerTask> task = std::make_unique<WarmJob>(warm);
        require(warmed.try_submit(task) == TaskSubmitResult::accepted, "warm task accepted");
    }
    {
        std::unique_lock<std::mutex> lock(warm.mutex);
        require(warm.cv.wait_for(lock,
                    std::chrono::seconds(5),
                    [&] {
                        return warm.ready == 4;
                    }),
            "all warm workers identified");
        warm.release = true;
    }
    warm.cv.notify_all();
    warmed.join();
    // pthread join与Linux /proc条目撤销不是同一次观测；核具体warm tid最终消失。
    require(observed_within([&] {
        for (const auto tid : warm.tids)
            if (std::filesystem::exists("/proc/self/task/" + std::to_string(tid)))
                return false;
        return true;
    }),
        "known warm worker tids disappear");
    const auto baseline_ids = task_ids();
    require(allocation_count >= 6, "observed slot vector and four thread states");
    // 包装实际构造者new调用，包括已成功创建部分worker后的下一线程状态分配。
    // 不冒称拦截libstdc++ DSO内部pthread_create或验证真实线程资源耗尽。
    for (unsigned int index = 1; index <= allocation_count; ++index) {
        allocations = 0;
        fail_at = index;
        observing = true;
        bool caught = false;
        try {
            WorkerPool pool(4, 3);
        } catch (const std::bad_alloc &) {
            caught = true;
        }
        observing = false;
        fail_at = 0;
        require(caught && allocations == index, "every startup allocation propagates");
        require(observed_within([&] {
            return task_ids() == baseline_ids;
        }),
            "partial startup returns to exact runtime thread baseline");
    }
    WorkerPool pool(2, 1);
    std::unique_ptr<WorkerTask> source = std::make_unique<Job>();
    const auto *identity = source.get();
    lock_failure([&] {
        (void)pool.try_submit(source);
    });
    require(source.get() == identity && pool.queue_status().size == 0, "submit error retains");
    lock_failure([&] {
        pool.request_stop();
    });
    require(!pool.queue_status().closed, "stop error unchanged");
    lock_failure([&] {
        (void)pool.report();
    });
    lock_failure([&] {
        pool.join();
    });
    require(!pool.queue_status().closed, "join mutex error unchanged");
    require(pool.try_submit(source) == TaskSubmitResult::accepted,
        "usable after recoverable initial locks");
    pool.join();
    require(pool.report().succeeded == 1, "normal join after failure");
    require(observed_within([&] {
        return task_ids() == baseline_ids;
    }),
        "final no worker tids retained");
    std::cout << "worker failure checks=" << checks << " startup_allocations=" << allocation_count
              << '\n';
}
