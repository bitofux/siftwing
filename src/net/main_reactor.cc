/*
 * PROJECT : SIFTWING
 * FILE    : main_reactor.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 固定目标轮询、预算接入、提交后失败停止与通知借用撤销
 */
#include "siftwing/net/main_reactor.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <array>
#include <cerrno>
#include <exception>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <new>
#include <sys/socket.h>
#include <utility>

namespace siftwing::net {
namespace {
std::error_code code(int value) noexcept {
    return {value, std::generic_category()};
}
void first(std::error_code &destination, std::error_code error) noexcept {
    if (!destination && error)
        destination = error;
}
std::error_code current_exception() noexcept {
    try {
        throw;
    } catch (const std::system_error &error) {
        return error.code();
    } catch (const std::bad_alloc &) {
        return code(ENOMEM);
    } catch (...) {
        return code(EIO);
    }
}
} // namespace
MainReactor::MainReactor(std::vector<MainReactorTarget> &&targets,
    Listener &&listener,
    UniqueFd &&epoll,
    UniqueFd &&wakeup,
    UniqueFd &&termination,
    MainReactorConfig config) noexcept
    : targets_(std::move(targets)), listener_(std::move(listener)), epoll_(std::move(epoll)),
      wakeup_(std::move(wakeup)), termination_(std::move(termination)), config_(config) {}
MainReactor::~MainReactor() noexcept {
    try {
        (void)close();
    } catch (...) {
        std::terminate();
    }
}
std::variant<std::unique_ptr<MainReactor>, std::error_code> MainReactor::create(
    const std::vector<MainReactorTarget> &targets,
    const ListenConfig &listen,
    const MainReactorConfig &config) {
    if (targets.empty() || targets.size() > 64 || config.accept_attempt_budget == 0
        || config.accept_attempt_budget > 1024)
        return code(EINVAL);
    // 完整结构预检先于任何queue加锁或I/O，防止非法后缀被部分创建掩盖。
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (!targets[i].queue || targets[i].wakeup_fd < 0)
            return code(EINVAL);
        for (std::size_t j = 0; j < i; ++j)
            if (targets[i].queue == targets[j].queue
                || targets[i].wakeup_fd == targets[j].wakeup_fd)
                return code(EINVAL);
    }
    for (const auto &target : targets)
        if (target.queue->status().closed)
            return code(ECANCELED);
    UniqueFd termination;
    if (config.termination_signals) {
        auto source = create_termination_signalfd();
        if (auto *error = std::get_if<std::error_code>(&source))
            return *error;
        termination = std::move(std::get<UniqueFd>(source));
    }
    auto table = targets;
    auto listening = Listener::create(listen);
    if (auto *error = std::get_if<std::error_code>(&listening))
        return *error;
    auto poller = create_epoll();
    if (auto *error = std::get_if<std::error_code>(&poller))
        return *error;
    auto notification = create_eventfd_wakeup();
    if (auto *error = std::get_if<std::error_code>(&notification))
        return *error;
    auto &listener = std::get<Listener>(listening);
    auto &epoll = std::get<UniqueFd>(poller);
    auto &wakeup = std::get<UniqueFd>(notification);
    if (const auto error = epoll_add(epoll.get(), listener.get(), EPOLLIN, 1))
        return error;
    if (const auto error = epoll_add(epoll.get(), wakeup.get(), EPOLLIN, 2))
        return error;
    if (termination)
        if (const auto error = epoll_add(epoll.get(), termination.get(), EPOLLIN, 3))
            return error;
    return std::unique_ptr<MainReactor>(new MainReactor(std::move(table),
        std::move(listener),
        std::move(epoll),
        std::move(wakeup),
        std::move(termination),
        config));
}
std::error_code MainReactor::request_stop() {
    stop_.store(true);
    const std::lock_guard<std::mutex> lock(wakeup_mutex_);
    return wakeup_ ? notify_eventfd(wakeup_.get()) : std::error_code{};
}
std::error_code MainReactor::close() {
    // 先撤销自己的通知借用；锁外close，迟到request_stop只看到空拥有者。
    UniqueFd notification;
    {
        const std::lock_guard<std::mutex> lock(wakeup_mutex_);
        notification = std::move(wakeup_);
    }
    if (closed_)
        return cleanup_error_;
    closed_ = true;
    stop_.store(true);
    for (const auto &target : targets_) {
        try {
            target.queue->close();
        } catch (...) {
            first(cleanup_error_, current_exception());
        }
    }
    if (listener_.get() >= 0)
        first(cleanup_error_, epoll_remove(epoll_.get(), listener_.get()));
    if (notification)
        first(cleanup_error_, epoll_remove(epoll_.get(), notification.get()));
    if (termination_)
        first(cleanup_error_, epoll_remove(epoll_.get(), termination_.get()));
    first(cleanup_error_, termination_.close());
    first(cleanup_error_, listener_.close());
    first(cleanup_error_, notification.close());
    first(cleanup_error_, epoll_.close());
    return cleanup_error_;
}
void MainReactor::fail(MainReactorStep &step, std::error_code error) {
    first(first_error_, error);
    step.stop = MainReactorStop::failed;
    step.error = first_error_;
    step.cleanup_error = close();
}
MainReactorStep MainReactor::run_once(int timeout_ms) {
    MainReactorStep step;
    if (closed_) {
        step.stop = first_error_ ? MainReactorStop::failed : MainReactorStop::closed;
        step.error = first_error_;
        step.cleanup_error = cleanup_error_;
        return step;
    }
    if (timeout_ms < 0 || timeout_ms > stop_poll_interval_ms) {
        step.stop = MainReactorStop::invalid_timeout;
        step.error = code(EINVAL);
        return step;
    }
    if (stop_.load()) {
        step.stop = MainReactorStop::closed;
        step.cleanup_error = close();
        return step;
    }
    bool target_closed = false;
    try {
        for (const auto &target : targets_) {
            if (target.queue->status().closed)
                target_closed = true;
        }
    } catch (...) {
        fail(step, current_exception());
        return step;
    }
    if (target_closed) {
        fail(step, code(ECANCELED));
        return step;
    }
    if (shutdown_signal_ != 0) {
        step.stop = MainReactorStop::shutdown_requested;
        step.signal_number = shutdown_signal_;
        return step;
    }
    std::array<epoll_event, 3> events{};
    const int count =
        ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), timeout_ms);
    // wait后的stop优先；即使通知失败或EINTR也不会继续接入。
    if (stop_.load()) {
        step.stop = MainReactorStop::closed;
        step.cleanup_error = close();
        return step;
    }
    if (count < 0) {
        const auto error = code(errno);
        if (error.value() == EINTR) {
            step.stop = MainReactorStop::interrupted;
            step.error = error;
        } else
            fail(step, error);
        return step;
    }
    if (count == 0) {
        step.stop = MainReactorStop::idle;
        return step;
    }
    step.events = static_cast<std::size_t>(count);
    bool accept_ready = false;
    for (int i = 0; i < count; ++i) {
        const auto &event = events[static_cast<std::size_t>(i)];
        if (event.events & (EPOLLERR | EPOLLHUP)) {
            fail(step, code(EIO));
            return step;
        }
        if (event.data.u64 == 1 && (event.events & EPOLLIN))
            accept_ready = true;
        if (event.data.u64 == 2) {
            const auto result = drain_eventfd(wakeup_.get());
            if (result.state == EventFdDrainState::failed) {
                fail(step, result.error);
                return step;
            }
        }
        if (event.data.u64 == 3) {
            const auto result = drain_termination_signalfd(termination_.get());
            if (result.state == SignalFdDrainState::failed) {
                fail(step, result.error);
                return step;
            }
            if (shutdown_signal_ == 0)
                shutdown_signal_ = result.signal_number;
        }
    }
    // 整批控制来源均处理成功才报告正常终止；同批监听就绪也不得再accept。
    if (stop_.load()) {
        step.stop = MainReactorStop::closed;
        step.cleanup_error = close();
        return step;
    }
    if (shutdown_signal_ != 0) {
        step.stop = MainReactorStop::shutdown_requested;
        step.signal_number = shutdown_signal_;
        return step;
    }
    if (!accept_ready)
        return step;
    while (step.accept_attempts < config_.accept_attempt_budget && !stop_.load()) {
        UniqueFd source;
        // 复用Listener已有pending network error/EINTR重试合同，只为一个拥有者提供槽位。
        const auto batch =
            listener_.accept_into(&source, 1, config_.accept_attempt_budget - step.accept_attempts);
        step.accept_attempts += batch.attempts;
        if (!source) {
            if (batch.stop == AcceptStop::error)
                fail(step, batch.error);
            step.accept_budget_exhausted = batch.stop == AcceptStop::budget_exhausted;
            break;
        }
        ++step.accepted;
        if (config_.tcp_no_delay) {
            const int enabled = 1;
            if (::setsockopt(source.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled))
                != 0) {
                const auto error = code(errno);
                ++step.rejected;
                first(cleanup_error_, source.close());
                fail(step, error);
                return step;
            }
        }
        const auto target = targets_[cursor_];
        // 移交尝试无论满/成功/锁异常都前进；配置失败在尝试前，不推进。
        cursor_ = cursor_ + 1 == targets_.size() ? 0 : cursor_ + 1;
        FdHandoffWakeupResult result;
        try {
            result = try_handoff_and_notify(*target.queue, source, target.wakeup_fd);
        } catch (...) {
            const auto error = current_exception();
            ++step.rejected;
            first(cleanup_error_, source.close());
            fail(step, error);
            return step;
        }
        if (result.handoff == FdHandoffResult::accepted) {
            ++step.handed_off;
            if (result.notification_error) {
                fail(step, result.notification_error);
                return step;
            }
        } else {
            ++step.rejected;
            first(cleanup_error_, source.close());
            if (result.handoff != FdHandoffResult::full) {
                fail(step, code(ECANCELED));
                return step;
            }
        }
    }
    if (!closed_ && stop_.load()) {
        step.stop = MainReactorStop::closed;
        step.cleanup_error = close();
    }
    if (step.accept_attempts == config_.accept_attempt_budget)
        step.accept_budget_exhausted = true;
    step.cleanup_error = cleanup_error_;
    return step;
}
} // namespace siftwing::net
