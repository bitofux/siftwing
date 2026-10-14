/*
 * PROJECT : SIFTWING
 * FILE    : sub_reactor_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 测试专属syscall注入验证所属线程清理、首因和部分启动回滚
 */
#include "siftwing/net/sub_reactor.hpp"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <sys/epoll.h>
#include <unistd.h>

namespace {
using namespace siftwing::net;
thread_local bool fail_allocation = false;
unsigned int checks = 0;
// 场景配置在线程启动前写入、join后再改；观测同样join后读取，无未同步全局读写。
int create_error = 0, wait_error = 0, close_error = 0, owned_fd = -1;
int wait_exception = 0;
unsigned int creates = 0, closes = 0, interrupts = 0;
std::atomic<unsigned int> waits{0};
std::thread::id creator, closer;
bool finite_wait = true;
std::mutex gate_mutex;
std::condition_variable gate_cv;
bool gate_create = false, gate_wait = false, entered = false, proceed = false;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "sub reactor injection failed: " << label << '\n';
        std::exit(1);
    }
}
void gate() {
    std::unique_lock<std::mutex> lock(gate_mutex);
    entered = true;
    gate_cv.notify_one();
    gate_cv.wait(lock, [] {
        return proceed;
    });
}
void wait_for_gate() {
    std::unique_lock<std::mutex> lock(gate_mutex);
    require(gate_cv.wait_for(lock,
                std::chrono::seconds(3),
                [] {
                    return entered;
                }),
        "injected barrier reached");
}
void release_gate() {
    std::lock_guard<std::mutex> lock(gate_mutex);
    proceed = true;
    gate_cv.notify_one();
}
void reset() {
    create_error = wait_error = close_error = 0;
    wait_exception = 0;
    owned_fd = -1;
    creates = closes = interrupts = 0;
    waits.store(0);
    creator = closer = {};
    finite_wait = true;
    gate_create = gate_wait = entered = proceed = false;
}
void require_closed() {
    require(creates == 1 && closes == 1, "exactly one create and close");
    require(creator != std::this_thread::get_id() && closer == creator,
        "resource created and closed by the same Sub thread");
    require(::fcntl(owned_fd, F_GETFD) < 0 && errno == EBADF, "kernel descriptor closed");
    require(finite_wait, "every wait uses bounded positive timeout");
}
} // namespace
extern "C" {
// Linux ELF测试链接时拦截operator new；保留真实分配器和sanitizer拦截器。
void *__real__Znwm(std::size_t);
void *__wrap__Znwm(std::size_t size) {
    if (fail_allocation) {
        fail_allocation = false;
        throw std::bad_alloc();
    }
    return __real__Znwm(size);
}
int __real_epoll_create1(int);
int __real_epoll_wait(int, epoll_event *, int, int);
int __real_close(int);
int __wrap_epoll_create1(int flags) {
    ++creates;
    creator = std::this_thread::get_id();
    if (gate_create)
        gate();
    if (create_error) {
        errno = create_error;
        return -1;
    }
    owned_fd = __real_epoll_create1(flags);
    return owned_fd;
}
int __wrap_epoll_wait(int fd, epoll_event *events, int size, int timeout) {
    waits.fetch_add(1);
    finite_wait = finite_wait && timeout == SubReactor::stop_poll_interval_ms;
    if (gate_wait)
        gate();
    // epoll_wait是非noexcept取消点；仅测试包装器抛出，核线程边界吸收三类异常。
    if (wait_exception == 1)
        throw std::system_error(EBADF, std::generic_category());
    if (wait_exception == 2)
        throw std::bad_alloc();
    if (wait_exception == 3)
        throw 7;
    if (interrupts > 0) {
        --interrupts;
        errno = EINTR;
        return -1;
    }
    if (wait_error) {
        errno = wait_error;
        return -1;
    }
    return __real_epoll_wait(fd, events, size, timeout);
}
int __wrap_close(int fd) {
    if (fd == owned_fd) {
        ++closes;
        closer = std::this_thread::get_id();
        const auto result = __real_close(fd);
        // 真实close先释放槽，再模拟Linux关闭诊断；不留下假泄漏或让重试误关复用fd。
        if (close_error) {
            errno = close_error;
            return -1;
        }
        return result;
    }
    return __real_close(fd);
}
}
int main() {
    for (const int failure : {EMFILE, ENFILE, ENOMEM, EINTR, EINVAL}) {
        reset();
        create_error = failure;
        SubReactor reactor;
        require(reactor.start() == std::error_code(failure, std::generic_category()),
            "creation errno preserved without retry");
        require(reactor.status().state == SubReactorState::joined && creates == 1 && closes == 0,
            "startup failure joined without partial resource");
        require(reactor.join().value() == failure, "failure remains after repeat join");
    }
    reset();
    {
        SubReactor reactor;
        fail_allocation = true;
        const auto error = reactor.start();
        fail_allocation = false;
        require(error == std::errc::not_enough_memory && creates == 0,
            "thread allocation failure creates no I/O resource");
        require(
            reactor.status().state == SubReactorState::joined, "failed thread creation terminal");
    }
    reset();
    {
        gate_create = true;
        close_error = EIO;
        SubReactor reactor;
        std::error_code result;
        std::thread controller([&] {
            result = reactor.start();
        });
        wait_for_gate();
        require(reactor.status().state == SubReactorState::starting, "no premature readiness");
        reactor.request_stop();
        release_gate();
        controller.join();
        require(result == std::errc::operation_canceled, "stop during creation cancels startup");
        require(reactor.status().state == SubReactorState::joined, "cancel return includes join");
        require(reactor.status().cleanup_error.value() == EIO,
            "startup cancellation keeps separate cleanup diagnostic");
        require_closed();
    }
    reset();
    {
        gate_wait = true;
        wait_error = EBADF;
        close_error = EIO;
        SubReactor reactor;
        require(!reactor.start(), "ready published before runtime failure");
        wait_for_gate();
        release_gate();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (reactor.status().state != SubReactorState::stopped
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(reactor.status().state == SubReactorState::stopped,
            "thread cleanup reported before external join");
        require(reactor.join().value() == EBADF, "runtime first cause wins over cleanup");
        const auto status = reactor.status();
        require(status.first_error.value() == EBADF && status.cleanup_error.value() == EIO,
            "both diagnostics retained separately");
        require_closed();
    }
    for (const int kind : {1, 2, 3}) {
        reset();
        gate_wait = true;
        wait_exception = kind;
        SubReactor reactor;
        require(!reactor.start(), "ready before injected thread exception");
        wait_for_gate();
        release_gate();
        const auto expected = kind == 1   ? std::make_error_code(std::errc::bad_file_descriptor)
                              : kind == 2 ? std::make_error_code(std::errc::not_enough_memory)
                                          : std::make_error_code(std::errc::io_error);
        require(reactor.join() == expected && reactor.status().first_error == expected,
            "thread boundary translates system, allocation and unknown exception");
        require_closed();
    }
    reset();
    {
        interrupts = 3;
        SubReactor reactor;
        require(!reactor.start(), "EINTR scenario ready");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (waits.load() < 4 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(waits.load() >= 4, "EINTR loops back to stop check and real wait");
        reactor.request_stop();
        require(!reactor.join(), "EINTR is not terminal error");
        require_closed();
    }
    reset();
    {
        close_error = EINTR;
        SubReactor reactor;
        require(!reactor.start(), "cleanup-only scenario ready");
        reactor.request_stop();
        require(reactor.join().value() == EINTR && !reactor.status().first_error,
            "cleanup-only error reported without fabricated runtime cause");
        require_closed();
    }
    reset();
    {
        SubReactor first, second;
        require(!first.start(), "first Sub ready");
        // 第一线程从不再调用create；此时改变配置只会由第二线程读取。
        create_error = EMFILE;
        require(second.start().value() == EMFILE, "second Sub startup fails");
        first.request_stop();
        require(!first.join(), "controller rolls back already-ready Sub");
        require(first.status().state == SubReactorState::joined
                    && second.status().state == SubReactorState::joined && closes == 1,
            "partial startup leaves no live Sub");
        require(::fcntl(owned_fd, F_GETFD) < 0 && errno == EBADF, "rollback closed first epoll");
    }
    std::cout << "checks=" << checks
              << " failures=0 test_only_wrap=epoll_create1,epoll_wait,close\n";
}
