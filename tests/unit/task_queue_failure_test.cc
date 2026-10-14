/*
 * PROJECT : SIFTWING
 * FILE    : task_queue_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 槽分配和提交前锁失败的拥有者保留、正常操作无分配
 */
#include "siftwing/concurrency/task_queue.hpp"
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <new>
#include <pthread.h>

namespace {
thread_local bool fail_allocation = false, fail_lock = false, observe_allocations = false;
thread_local unsigned int allocations = 0;
unsigned int checks = 0, destroyed = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << "task failure: " << label << '\n';
        std::exit(1);
    }
}
struct Job {
    int value = 42;
    ~Job() noexcept {
        ++destroyed;
    }
};
using Queue = siftwing::concurrency::TaskQueue<Job>;
using Result = siftwing::concurrency::TaskSubmitResult;
template <class Action>
void lock_failure(Action action) {
    fail_lock = true;
    bool caught = false;
    try {
        action();
    } catch (const std::system_error &error) {
        caught = error.code().value() == EINVAL;
    }
    require(!fail_lock && caught, "initial lock failure propagated");
}
} // namespace
// 同编译单元的mutex入口可包装；不声称拦截libstdc++ DSO内部condition_variable等待。
extern "C" {
void *__real__Znwm(std::size_t);
void *__wrap__Znwm(std::size_t size) {
    if (observe_allocations)
        ++allocations;
    if (fail_allocation) {
        fail_allocation = false;
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
    fail_allocation = true;
    bool caught = false;
    try {
        Queue queue(7);
    } catch (const std::bad_alloc &) {
        caught = true;
    }
    require(caught && !fail_allocation, "initial slot allocation failure");
    Queue queue(1);
    auto source = std::make_unique<Job>();
    auto *identity = source.get();
    lock_failure([&] {
        (void)queue.try_submit(source);
    });
    require(
        source.get() == identity && queue.status().size == 0, "submit lock failure keeps owner");
    lock_failure([&] {
        queue.close();
    });
    require(!queue.status().closed, "close lock failure keeps state");
    lock_failure([&] {
        (void)queue.status();
    });
    require(queue.try_submit(source) == Result::accepted && !source, "normal commit after failure");
    lock_failure([&] {
        (void)queue.try_pop();
    });
    require(queue.status().size == 1 && destroyed == 0, "pop failure keeps task");
    lock_failure([&] {
        (void)queue.wait_pop();
    });
    require(queue.status().size == 1 && destroyed == 0, "wait initial lock failure keeps task");
    // 全部用户任务事先创建；计数只观察queue正常路径，不把调用者分配混进证据。
    auto overflow = std::make_unique<Job>();
    auto *overflow_identity = overflow.get();
    observe_allocations = true;
    const auto full = queue.try_submit(overflow);
    auto item = queue.wait_pop();
    const auto accepted = queue.try_submit(overflow);
    queue.close();
    auto last = queue.wait_pop();
    const auto finished = queue.wait_pop();
    const auto status = queue.status();
    observe_allocations = false;
    require(full == Result::full && accepted == Result::accepted, "full/refill results");
    require(item.get() == identity && last.get() == overflow_identity && !finished && !overflow,
        "exact ownership despite failures");
    require(status.closed && status.size == 0 && allocations == 0 && destroyed == 0,
        "normal queue operations allocate/destroy no tasks");
    item.reset();
    last.reset();
    require(destroyed == 2, "only consumers destroy tasks");
    source = std::make_unique<Job>();
    identity = source.get();
    require(queue.try_submit(source) == Result::closed && source.get() == identity,
        "closed retains owner after injected failures");
    std::cout << "task injection checks=" << checks << '\n';
}
