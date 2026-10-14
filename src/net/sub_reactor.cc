/*
 * PROJECT : SIFTWING
 * FILE    : sub_reactor.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : Sub线程启动报告、有限epoll等待和首因不被清理错误覆盖
 */
#include "siftwing/net/sub_reactor.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <cerrno>
#include <new>
#include <utility>

namespace siftwing::net {
namespace {
// 仅在catch内调用；线程边界不让未知异常逃出，避免std::thread直接terminate。
std::error_code current_exception_error() noexcept {
    try {
        throw;
    } catch (const std::system_error &error) {
        return error.code();
    } catch (const std::bad_alloc &) {
        return std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        return std::make_error_code(std::errc::io_error);
    }
}
} // namespace
SubReactor::~SubReactor() noexcept {
    request_stop();
    (void)join();
}
std::error_code SubReactor::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SubReactorState::idle)
            return {EALREADY, std::generic_category()};
        if (stop_.load(std::memory_order_acquire)) {
            first_error_ = std::make_error_code(std::errc::operation_canceled);
            state_ = SubReactorState::joined;
            return first_error_;
        }
        state_ = SubReactorState::starting;
    }
    try {
        thread_ = std::thread(&SubReactor::run, this);
    } catch (...) {
        const auto error = current_exception_error();
        std::lock_guard<std::mutex> lock(mutex_);
        first_error_ = error;
        state_ = SubReactorState::joined;
        return error;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    ready_cv_.wait(lock, [this] {
        return ready_;
    });
    const auto error = ready_error_;
    lock.unlock();
    // 启动失败的返回值同时保证没有未回收线程，持锁join会阻碍线程发布清理结果。
    if (error)
        (void)join();
    return error;
}
void SubReactor::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
}
std::error_code SubReactor::join() {
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id())
            return std::make_error_code(std::errc::resource_deadlock_would_occur);
        thread_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = SubReactorState::joined;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return first_error_ ? first_error_ : cleanup_error_;
}
SubReactorStatus SubReactor::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {state_, stop_.load(std::memory_order_acquire), first_error_, cleanup_error_};
}
void SubReactor::run() noexcept {
    // I/O拥有者是线程栈局部对象；控制线程从未持有或关闭此fd。
    UniqueFd epoll;
    std::error_code failure;
    try {
        if (stop_.load(std::memory_order_acquire)) {
            failure = std::make_error_code(std::errc::operation_canceled);
        } else {
            auto created = create_epoll();
            if (const auto *error = std::get_if<std::error_code>(&created))
                failure = *error;
            else
                epoll = std::move(std::get<UniqueFd>(created));
        }
        if (!failure && stop_.load(std::memory_order_acquire))
            failure = std::make_error_code(std::errc::operation_canceled);
        if (!failure) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = SubReactorState::running;
                ready_ = true;
            }
            ready_cv_.notify_one();
            epoll_event event{};
            while (!stop_.load(std::memory_order_acquire)) {
                // 当前没有注册项；有限等待仅为停止后备路径，不实现连接分发或通知。
                const int count = ::epoll_wait(epoll.get(), &event, 1, stop_poll_interval_ms);
                if (count < 0 && errno != EINTR) {
                    failure = {errno, std::generic_category()};
                    break;
                }
            }
        }
    } catch (...) {
        failure = current_exception_error();
    }
    const auto cleanup = epoll.close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        first_error_ = failure;
        cleanup_error_ = cleanup;
        state_ = SubReactorState::stopped;
        if (!ready_) {
            ready_error_ = failure;
            ready_ = true;
        }
    }
    ready_cv_.notify_one();
}
} // namespace siftwing::net
