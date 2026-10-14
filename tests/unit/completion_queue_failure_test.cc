/*
 * PROJECT : SIFTWING
 * FILE    : completion_queue_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 预分配与初次mutex失败保持、提交及取出零分配
 */
#include "siftwing/concurrency/completion_queue.hpp"
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <new>
#include <pthread.h>
#include <system_error>
namespace {
using namespace siftwing::concurrency;
using namespace siftwing::net;
unsigned int checks = 0;
thread_local bool observe = false, fail_new = false, fail_lock = false;
thread_local unsigned int allocations = 0;
void require(bool value, const char *why) {
    ++checks;
    if (!value) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
std::unique_ptr<RoutedCompletion> output() {
    Frame frame{MessageType::search_request, 0, 77, "{}"};
    auto source_result = SearchTask::create(frame, 128);
    auto source = std::get<SearchTask>(std::move(source_result));
    auto task_result = RoutedSearchTask::create({7, 0, 1}, source);
    auto task = std::get<RoutedSearchTask>(std::move(task_result));
    auto result = RoutedCompletion::success(task, "{\"owned\":true}");
    return std::make_unique<RoutedCompletion>(std::get<RoutedCompletion>(std::move(result)));
}
template <class Operation>
void lock_failure(Operation operation) {
    fail_lock = true;
    bool caught = false;
    try {
        operation();
    } catch (const std::system_error &error) {
        caught = error.code().value() == EAGAIN;
    }
    require(caught && !fail_lock, "initial lock failure propagated");
}
}
extern "C" void *__real__Znwm(std::size_t);
extern "C" void *__wrap__Znwm(std::size_t n) {
    if (observe) {
        ++allocations;
        if (fail_new) {
            fail_new = false;
            throw std::bad_alloc();
        }
    }
    return __real__Znwm(n);
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (fail_lock) {
        fail_lock = false;
        return EAGAIN;
    }
    return __real_pthread_mutex_lock(mutex);
}
int main() {
    observe = true;
    fail_new = true;
    allocations = 0;
    bool caught = false;
    try {
        CompletionQueue invalid(7, 8);
    } catch (const std::bad_alloc &) {
        caught = true;
    }
    observe = false;
    require(caught && allocations == 1 && !fail_new, "preallocation failure propagates");
    CompletionQueue queue(7, 1);
    auto source = output();
    const auto *pointer = source.get();
    const auto body = source->payload()->frame()->body;
    lock_failure([&] {
        (void)queue.try_publish(source);
    });
    require(source.get() == pointer && source->payload()->frame()->body == body
                && queue.status().size == 0,
        "publish lock failure keeps source and queue");
    observe = true;
    allocations = 0;
    const auto published = queue.try_publish(source);
    observe = false;
    require(published == CompletionPublishResult::accepted && !source && allocations == 0,
        "publish commit has zero allocation");
    lock_failure([&] {
        (void)queue.try_pop();
    });
    require(queue.status().size == 1, "pop lock failure keeps result");
    lock_failure([&] {
        queue.close();
    });
    require(!queue.status().closed && queue.status().size == 1, "close lock failure keeps state");
    lock_failure([&] {
        (void)queue.status();
    });
    require(queue.status().size == 1, "observation lock failure cannot alter state");
    auto second = output();
    const auto *second_pointer = second.get();
    observe = true;
    allocations = 0;
    const auto full = queue.try_publish(second);
    queue.close();
    const auto state = queue.status();
    const auto closed = queue.try_publish(second);
    auto popped = queue.try_pop();
    auto empty = queue.try_pop();
    observe = false;
    require(allocations == 0 && full == CompletionPublishResult::full
                && closed == CompletionPublishResult::closed && state.closed && state.size == 1,
        "refusal, close, observation and drain allocate nothing");
    require(second.get() == second_pointer && popped.get() == pointer && !empty
                && popped->payload()->frame()->body == body,
        "exact owners preserved");
    // 故意保留一个已接收结果到析构，ASan/LSan检查兜底释放，无活跃发布者。
    {
        CompletionQueue leftovers(7, 1);
        auto value = output();
        require(leftovers.try_publish(value) == CompletionPublishResult::accepted,
            "destructor owns residual result");
    }
    std::cout << "completion failure checks=" << checks << '\n';
}
