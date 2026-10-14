/*
 * PROJECT : SIFTWING
 * FILE    : fd_handoff_queue_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 分配与加锁失败保留源、无分配操作及锁外所属线程关闭
 */
#include "siftwing/net/fd_handoff_queue.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <pthread.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {
using namespace siftwing::net;
thread_local bool fail_new = false, count_new = false;
thread_local unsigned int allocations = 0;
thread_local int lock_error = 0;
unsigned int checks = 0, closes = 0;
int watched = -1, close_error = 0;
std::thread::id closing_thread;
FdHandoffQueue *probe = nullptr;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "fd handoff injection failed: " << label << '\n';
        std::exit(1);
    }
}
UniqueFd descriptor() {
    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    require(static_cast<bool>(fd), "open resource");
    return fd;
}
template<class Action> void fails_before_commit(Action action) {
    lock_error = EAGAIN;
    bool failed = false;
    try { action(); } catch (const std::system_error &error) { failed = error.code().value() == EAGAIN; }
    require(failed && lock_error == 0, "mutex failure reported");
}
} // namespace
extern "C" {
void *__real__Znwm(std::size_t);
void *__wrap__Znwm(std::size_t size) {
    if (count_new) ++allocations;
    if (fail_new) { fail_new = false; throw std::bad_alloc(); }
    return __real__Znwm(size);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (lock_error) { const auto error = lock_error; lock_error = 0; return error; }
    return __real_pthread_mutex_lock(mutex);
}
int __real_close(int);
int __wrap_close(int fd) {
    if (fd == watched) {
        ++closes; closing_thread = std::this_thread::get_id();
        // 重入status会获取队列锁；若资源清理错误地位于锁内，真实CTest期限会暴露死锁。
        if (probe) (void)probe->status();
        const auto result = __real_close(fd);
        if (close_error) { errno = close_error; return -1; }
        return result;
    }
    return __real_close(fd);
}
}
int main() {
    fail_new = true; bool failed = false;
    try { FdHandoffQueue queue(4); } catch (const std::bad_alloc &) { failed = true; }
    require(failed && !fail_new, "constructor allocation failure propagated");
    FdHandoffQueue queue(1); auto source = descriptor();
    const int fd = source.get();
    watched = fd; closes = 0; probe = &queue;
    fails_before_commit([&] { (void)queue.try_push(source); });
    require(source.get() == fd && queue.status().size == 0, "push lock failure preserves owner and state");
    require(queue.try_push(source) == FdHandoffResult::accepted, "commit after failure");
    fails_before_commit([&] { (void)queue.try_pop(); });
    require(queue.status().size == 1, "pop lock failure retains entry");
    fails_before_commit([&] { queue.close(); });
    require(!queue.status().closed && queue.status().size == 1, "close lock failure leaves open state");
    fails_before_commit([&] { (void)queue.status(); });
    require(queue.status().size == 1, "observation failure leaves entry");
    auto extra = descriptor(); const int retained = extra.get();
    allocations = 0; count_new = true;
    const auto full = queue.try_push(extra);
    const auto state = queue.status();
    auto item = queue.try_pop();
    const auto empty = queue.try_pop();
    const auto again = queue.try_push(extra);
    queue.close(); queue.close();
    const auto closed = queue.try_push(source);
    auto last = queue.try_pop();
    count_new = false;
    require(allocations == 0, "all constructed queue operations allocate nothing");
    require(full == FdHandoffResult::full && state.size == 1 && !empty, "full and empty branches exercised");
    require(again == FdHandoffResult::accepted && closed == FdHandoffResult::closed && !extra,
        "successful and closed branches exercised");
    require(item && item->get() == fd && last && last->get() == retained, "failure paths preserve FIFO");
    require(closes == 0, "push/pop/close never close an owned descriptor internally");
    // 观测先于线程启动配置、join后读取；包装器不与控制线程并发读写这些观测字段。
    watched = fd; closes = 0; close_error = EINTR; probe = &queue;
    std::error_code error;
    std::thread::id target_id;
    std::thread target([&] { target_id = std::this_thread::get_id(); error = item->close(); });
    target.join();
    require(error.value() == EINTR && closes == 1 && closing_thread == target_id,
        "target close outside lock once even on EINTR");
    require(!item->close() && closes == 1 && ::fcntl(fd, F_GETFD) < 0 && errno == EBADF,
        "empty owner cannot retry released fd");
    probe = nullptr; close_error = 0; watched = -1;
    // 真正排队后由目标线程drain，检查队列锁内没有隐式close、析构也不重复关闭。
    auto queued = descriptor(); watched = queued.get(); closes = 0;
    {
        FdHandoffQueue pending(1); probe = &pending;
        require(pending.try_push(queued) == FdHandoffResult::accepted && closes == 0, "no close on push");
        pending.close(); require(closes == 0 && pending.status().size == 1, "no close on entry shutdown");
        std::thread consumer([&] {
            target_id = std::this_thread::get_id();
            while (auto entry = pending.try_pop()) { if (entry->close()) std::exit(2); }
        });
        consumer.join(); probe = nullptr;
        require(closes == 1 && closing_thread == target_id, "queued residue closed by target consumer");
    }
    require(closes == 1, "queue destruction does not double-close drained entry");
    auto fallback = descriptor(); watched = fallback.get(); closes = 0;
    {
        FdHandoffQueue abandoned(1);
        require(abandoned.try_push(fallback) == FdHandoffResult::accepted, "fallback entry committed");
    }
    require(closes == 1 && closing_thread == std::this_thread::get_id(), "destructor fallback is calling-thread cleanup");
    watched = -1;
    std::cout << "fd handoff injection checks=" << checks << '\n';
}
