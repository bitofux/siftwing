/*
 * PROJECT : SIFTWING
 * FILE    : protocol_sub_reactor.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 固定Connection归属、外层队列调度、线程生命周期与发布者静止收口
 */
#include "siftwing/net/protocol_sub_reactor.hpp"
#include "protocol_connection_core.hpp"
#include "siftwing/net/deadline_timer.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <cerrno>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
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
std::error_code exception_code() noexcept {
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
void increment_counter(std::size_t &value) noexcept {
    if (value != std::numeric_limits<std::size_t>::max())
        ++value;
}
ProtocolSubConfig checked(ProtocolSubConfig config) {
    const auto limit = frame_header_bytes + frame_max_body_bytes;
    if (config.handoff_capacity == 0 || config.handoff_capacity > 1024
        || config.max_connections == 0 || config.max_connections > 1024 || config.max_body_bytes < 2
        || config.max_body_bytes > frame_max_body_bytes || config.handoff_budget == 0
        || config.handoff_budget > 1024 || config.read_byte_budget == 0
        || config.read_byte_budget > limit || config.write_byte_budget == 0
        || config.write_byte_budget > limit || config.io_attempt_budget == 0
        || config.io_attempt_budget > 1024 || config.completion_capacity == 0
        || config.completion_capacity > 1024 || config.completion_budget == 0
        || config.completion_budget > 1024 || config.idle_timeout_ms < 0
        || config.idle_timeout_ms > 86400000 || config.request_timeout_ms < 0
        || config.request_timeout_ms > 86400000)
        throw std::invalid_argument("invalid ProtocolSubConfig");
    if (!config.server_info_body.empty()
        && validate_frame({MessageType::server_info_response, 0, 1, config.server_info_body},
            ReceiveRole::client))
        throw std::invalid_argument("invalid server info JSON");
    config.output = checked_output_watermarks(config.max_body_bytes, config.output);
    return config;
}
} // namespace

ProtocolSubReactor::ProtocolSubReactor(const ProtocolSubConfig &config)
    : config_(checked(config)), queue_(config_.handoff_capacity),
      completions_(config_.reactor_id, config_.completion_capacity) {}
ProtocolSubReactor::~ProtocolSubReactor() noexcept {
    (void)request_stop();
    release_publishers();
    (void)join();
}
std::error_code ProtocolSubReactor::start() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (report_.state != ProtocolSubState::idle)
            return code(EALREADY);
        if (stop_.load()) {
            report_.state = ProtocolSubState::joined;
            report_.first_error = code(ECANCELED);
            return report_.first_error;
        }
        report_.state = ProtocolSubState::starting;
    }
    try {
        thread_ = std::thread(&ProtocolSubReactor::run, this);
    } catch (...) {
        const auto error = exception_code();
        const std::lock_guard<std::mutex> lock(mutex_);
        report_.first_error = error;
        report_.state = ProtocolSubState::joined;
        return error;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] {
        return ready_done_;
    });
    const auto error = ready_error_;
    lock.unlock();
    if (error)
        (void)join();
    return error;
}
std::variant<MainReactorTarget, std::error_code> ProtocolSubReactor::target() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (report_.state != ProtocolSubState::running || publishers_released_ || borrowed_wakeup_ < 0)
        return code(ECANCELED);
    // 目标仅授予同步发布能力；queue的消费与所有Connection仍只在Sub线程。
    return MainReactorTarget{&queue_, borrowed_wakeup_};
}
std::error_code ProtocolSubReactor::request_stop() {
    stop_.store(true);
    const std::lock_guard<std::mutex> lock(mutex_);
    return borrowed_wakeup_ >= 0 ? notify_eventfd(borrowed_wakeup_) : std::error_code{};
}
std::error_code ProtocolSubReactor::request_drain(std::int64_t deadline) {
    if (deadline <= 0)
        return code(EINVAL);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stop_.load() || publishers_released_
        || (report_.state != ProtocolSubState::running
            && report_.state != ProtocolSubState::draining))
        return code(ECANCELED);
    const auto previous = drain_deadline_.load();
    if (!previous || deadline < previous)
        drain_deadline_.store(deadline);
    return borrowed_wakeup_ >= 0 ? notify_eventfd(borrowed_wakeup_) : std::error_code{};
}
std::variant<CompletionTarget, std::error_code> ProtocolSubReactor::completion_target() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (report_.state != ProtocolSubState::running || publishers_released_ || borrowed_wakeup_ < 0)
        return code(ECANCELED);
    return CompletionTarget{&completions_, borrowed_wakeup_};
}
void ProtocolSubReactor::release_publishers() {
    stop_.store(true);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        publishers_released_ = true;
    }
    cv_.notify_one();
}
std::error_code ProtocolSubReactor::join() {
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id())
            return code(EDEADLK);
        thread_.join();
        const std::lock_guard<std::mutex> lock(mutex_);
        report_.state = ProtocolSubState::joined;
    }
    const auto value = status();
    return value.first_error ? value.first_error : value.cleanup_error;
}
ProtocolSubStatus ProtocolSubReactor::status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto result = report_;
    result.stop_requested = stop_.load();
    return result;
}
SubQueueDepths ProtocolSubReactor::queue_depths() const {
    const auto handoff = queue_.status();
    const auto completion = completions_.status();
    return {handoff.size, completion.size};
}
void ProtocolSubReactor::run() noexcept {
    UniqueFd epoll, wakeup, timer;
    std::optional<ProtocolConnectionCore> sessions;
    ProtocolSubStatus local;
    bool ready = false, wakeup_registered = false, timer_registered = false;
    try {
        if (stop_.load())
            throw std::system_error(code(ECANCELED));
        auto poller = create_epoll();
        if (auto *error = std::get_if<std::error_code>(&poller))
            throw std::system_error(*error);
        epoll = std::move(std::get<UniqueFd>(poller));
        auto notification = create_eventfd_wakeup();
        if (auto *error = std::get_if<std::error_code>(&notification))
            throw std::system_error(*error);
        wakeup = std::move(std::get<UniqueFd>(notification));
        auto deadline = create_deadline_timer();
        if (auto *error = std::get_if<std::error_code>(&deadline))
            throw std::system_error(*error);
        timer = std::move(std::get<UniqueFd>(deadline));
        sessions.emplace(epoll.get(), timer.get(), config_, local, drain_deadline_);
        if (const auto error = epoll_add(epoll.get(), wakeup.get(), EPOLLIN, 0))
            throw std::system_error(error);
        wakeup_registered = true;
        if (const auto error = epoll_add(epoll.get(), timer.get(), EPOLLIN, 0))
            throw std::system_error(error);
        timer_registered = true;
        if (stop_.load())
            throw std::system_error(code(ECANCELED));
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            local.state = ProtocolSubState::running;
            report_ = local;
            borrowed_wakeup_ = wakeup.get();
            ready_done_ = true;
            ready = true;
        }
        cv_.notify_one();
        while (!stop_.load()) {
            // Main::close会关闭handoff；排空请求须先发布，再关闭Main，不能把closed误作立即停止。
            if (completions_.status().closed || (queue_.status().closed && !drain_deadline_.load()))
                break;
            if (drain_deadline_.load()) {
                // 未注册的移交源也必须立即结束网络等待，不能等可能无限阻塞的worker join。
                // 先关入口后有限清空（至多handoff_capacity），完整通知fd仍保留到发布者屏障。
                queue_.close();
                while (auto source = queue_.try_pop()) {
                    increment_counter(local.discarded);
                    first(local.cleanup_error, source->close());
                }
            }
            if (sessions->drained())
                break;
            const auto notification_result = drain_eventfd(wakeup.get());
            if (notification_result.state == EventFdDrainState::failed)
                throw std::system_error(notification_result.error);
            for (std::size_t n = 0;
                 n < config_.handoff_budget && !stop_.load() && !drain_deadline_.load();
                 ++n) {
                auto source = queue_.try_pop();
                if (!source)
                    break;
                increment_counter(local.received);
                try {
                    sessions->admit(*source);
                } catch (...) {
                    increment_counter(local.rejected);
                    first(local.cleanup_error, source->close());
                    throw;
                }
            }
            if (stop_.load())
                break;
            for (std::size_t n = 0; n < config_.completion_budget && !stop_.load(); ++n) {
                auto result = completions_.try_pop();
                if (!result)
                    break;
                sessions->complete(*result); // pop已释放queue锁，应用及销毁结果均在锁外。
            }
            if (stop_.load())
                break;
            const bool continue_work =
                (!drain_deadline_.load() && queue_.status().size) || completions_.status().size
                || notification_result.state == EventFdDrainState::budget_exhausted;
            sessions->tick(continue_work ? 0 : stop_poll_interval_ms);
            sessions->observe_metrics();
            const std::lock_guard<std::mutex> lock(mutex_);
            report_ = local;
        }
    } catch (...) {
        first(local.first_error, exception_code());
    }
    if (sessions)
        sessions->observe_metrics();
    // 运行首因先发布，通知端保持到外部生产者全部静止；closed/empty绝不是此屏障。
    stop_.store(true);
    {
        // 先发布运行首因再关队列，避免Main先观察closed而只保留ECANCELED。
        const std::lock_guard<std::mutex> lock(mutex_);
        local.state = ProtocolSubState::quiescing;
        report_ = local;
    }
    queue_.close();
    completions_.close();
    if (ready) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
            return publishers_released_;
        });
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        borrowed_wakeup_ = -1;
    }
    if (sessions) {
        sessions->close();
        sessions->observe_metrics();
        sessions.reset(); // 撤销timer/epoll/config/report借用后再关闭所属线程资源。
    }
    // 最终同步设施无法工作时不能悄悄把残留fd留到控制者析构；noexcept收口明确terminate。
    while (auto source = queue_.try_pop()) {
        increment_counter(local.discarded);
        first(local.cleanup_error, source->close());
    }
    while (auto result = completions_.try_pop())
        increment_counter(local.completion_discarded); // 静止后只销毁，不应用停止期间的结果。
    if (timer_registered)
        first(local.cleanup_error, epoll_remove(epoll.get(), timer.get()));
    first(local.cleanup_error, timer.close());
    if (wakeup_registered)
        first(local.cleanup_error, epoll_remove(epoll.get(), wakeup.get()));
    first(local.cleanup_error, wakeup.close());
    first(local.cleanup_error, epoll.close());
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        local.state = ProtocolSubState::stopped;
        report_ = local;
        if (!ready) {
            ready_error_ = local.first_error;
            ready_done_ = true;
        }
    }
    cv_.notify_one();
}
} // namespace siftwing::net
