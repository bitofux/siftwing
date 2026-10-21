/*
 * PROJECT : SIFTWING
 * FILE    : connection_harness.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-21
 * BRIEF   : 直接驱动真实连接核心的测试夹具；不进入生产库或提供旧主从API兼容
 */
#pragma once
#include "net_test_support.hpp"
#include "../../src/net/protocol_connection_core.hpp"
#include "siftwing/net/deadline_timer.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

namespace siftwing::test {
/**
 * @brief 捕获提交器/socketpair故障测试的单核心驱动器；生产拓扑另用真实OnlineServer验证
 * @note 客户端/故障注入线程之外仅一个测试I/O线程。adopt只注入测试socket，非生产fd交接队列。
 *     测试负责在release_publishers前等待全部外部发布/通知返回；不拥有submitter或worker。
 *     运行报告通过mutex复制，核心借用均在线程栈内直到清理完成；不复用已退役类或其代码入口。
 */
class ConnectionHarness final {
    ProtocolConnectionConfig config_;
    concurrency::CompletionQueue completions_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<UniqueFd> inputs_; // 最多1024个测试输入，无生产调用方或fd队列接口。
    ReactorReport report_;
    std::atomic<bool> stop_{false};
    std::atomic<std::int64_t> deadline_{0};
    bool started_ = false, ready_ = false, released_ = false;
    int wakeup_ = -1;
    std::uint16_t port_ = 0;
    std::error_code startup_error_;

    static std::error_code code(int value) {
        return {value, std::generic_category()};
    }
    static void first(std::error_code &target, std::error_code error) {
        if (!target)
            target = error;
    }
    static std::error_code caught() {
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
    static ProtocolConnectionConfig checked(ProtocolConnectionConfig config) {
        const auto maximum = frame_header_bytes + frame_max_body_bytes;
        if (!config.max_connections || config.max_connections > 65536 || !config.completion_capacity
            || config.completion_capacity > 65536 || !config.completion_budget
            || config.completion_budget > 1024 || config.max_body_bytes < 2
            || config.max_body_bytes > frame_max_body_bytes || !config.read_byte_budget
            || config.read_byte_budget > maximum || !config.write_byte_budget
            || config.write_byte_budget > maximum || !config.io_attempt_budget
            || config.io_attempt_budget > 1024 || config.idle_timeout_ms < 0
            || config.idle_timeout_ms > 86400000 || config.request_timeout_ms < 0
            || config.request_timeout_ms > 86400000)
            throw std::invalid_argument("invalid connection harness configuration");
        if (!config.server_info_body.empty()
            && validate_frame({MessageType::server_info_response, 0, 1, config.server_info_body},
                ReceiveRole::client))
            throw std::invalid_argument("invalid server info");
        config.output = checked_output_watermarks(config.max_body_bytes, config.output);
        return config;
    }
    static UniqueFd take(EventFdResult result) {
        if (const auto *error = std::get_if<std::error_code>(&result))
            throw std::system_error(*error);
        return std::move(std::get<UniqueFd>(result));
    }
    void run(bool tcp) noexcept {
        UniqueFd epoll, notification, timer;
        Listener listener;
        ReactorReport local;
        std::optional<ProtocolConnectionCore> core;
        bool ready = false, notification_registered = false, timer_registered = false,
             listener_registered = false;
        try {
            epoll = take(create_epoll());
            notification = take(create_eventfd_wakeup());
            timer = take(create_deadline_timer());
            core.emplace(epoll.get(), timer.get(), config_, local, deadline_);
            if (auto error = epoll_add(epoll.get(), notification.get(), EPOLLIN, 0))
                throw std::system_error(error);
            notification_registered = true;
            if (auto error = epoll_add(epoll.get(), timer.get(), EPOLLIN, 0))
                throw std::system_error(error);
            timer_registered = true;
            if (tcp) {
                auto created = Listener::create({});
                if (auto *error = std::get_if<std::error_code>(&created))
                    throw std::system_error(*error);
                listener = std::move(std::get<Listener>(created));
                if (auto error = epoll_add(epoll.get(), listener.get(), EPOLLIN, 0))
                    throw std::system_error(error);
                listener_registered = true;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                local.state = ReactorState::running;
                report_ = local;
                wakeup_ = notification.get();
                port_ = listener.port();
                ready_ = ready = true;
            }
            cv_.notify_one();
            while (!stop_.load() && !completions_.status().closed) {
                if (core->drained())
                    break;
                const auto notified = drain_eventfd(notification.get());
                if (notified.state == EventFdDrainState::failed)
                    throw std::system_error(notified.error);
                std::vector<UniqueFd> inputs;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    inputs.swap(inputs_);
                }
                for (auto &source : inputs) {
                    if (deadline_.load() || stop_.load())
                        continue;
                    ++local.received;
                    core->admit(source);
                }
                if (tcp && !deadline_.load()) {
                    for (unsigned attempt = 0; attempt < 16; ++attempt) {
                        auto accepted = listener.accept_one();
                        if (const auto *error = std::get_if<std::error_code>(&accepted)) {
                            if (error->value() == EAGAIN || error->value() == EWOULDBLOCK)
                                break;
                            if (error->value() == EINTR)
                                continue;
                            throw std::system_error(*error);
                        }
                        auto source = std::move(std::get<UniqueFd>(accepted));
                        ++local.received;
                        core->admit(source);
                    }
                }
                for (std::size_t n = 0; n < config_.completion_budget && !stop_.load(); ++n) {
                    auto result = completions_.try_pop();
                    if (!result)
                        break;
                    core->complete(*result);
                }
                if (stop_.load())
                    break;
                const bool work = core->prepare_round() || completions_.status().size
                                  || notified.state == EventFdDrainState::budget_exhausted;
                std::array<epoll_event, 64> events{};
                const int count = ::epoll_wait(epoll.get(), events.data(), 64, work ? 0 : 100);
                if (count < 0 && errno != EINTR)
                    throw std::system_error(code(errno));
                core->check_deadlines();
                for (int index = 0; index < count; ++index) {
                    const auto &event = events[static_cast<std::size_t>(index)];
                    if (!event.data.u64) {
                        const auto drained = drain_deadline_timer(timer.get());
                        if (drained.state == TimerDrainState::failed)
                            throw std::system_error(drained.error);
                    } else
                        core->dispatch(event.data.u64, event.events);
                }
                core->observe_metrics();
                const std::lock_guard<std::mutex> lock(mutex_);
                report_ = local;
            }
        } catch (...) {
            first(local.first_error, caught());
        }
        stop_.store(true);
        if (core)
            core->observe_metrics();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            local.state = ReactorState::quiescing;
            report_ = local;
        }
        completions_.close();
        if (ready) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return released_;
            });
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            wakeup_ = -1;
            inputs_.clear();
        }
        if (core) {
            core->close();
            core->observe_metrics();
            core.reset();
        }
        while (auto result = completions_.try_pop())
            ++local.completion_discarded;
        if (listener_registered)
            first(local.cleanup_error, epoll_remove(epoll.get(), listener.get()));
        first(local.cleanup_error, listener.close());
        if (timer_registered)
            first(local.cleanup_error, epoll_remove(epoll.get(), timer.get()));
        first(local.cleanup_error, timer.close());
        if (notification_registered)
            first(local.cleanup_error, epoll_remove(epoll.get(), notification.get()));
        first(local.cleanup_error, notification.close());
        first(local.cleanup_error, epoll.close());
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            local.state = ReactorState::stopped;
            report_ = local;
            if (!ready) {
                startup_error_ = local.first_error;
                ready_ = true;
            }
        }
        cv_.notify_one();
    }

public:
    explicit ConnectionHarness(ProtocolConnectionConfig config = {})
        : config_(checked(std::move(config))),
          completions_(config_.reactor_id, config_.completion_capacity) {}
    ~ConnectionHarness() noexcept {
        (void)stop();
    }
    ConnectionHarness(const ConnectionHarness &) = delete;
    ConnectionHarness &operator=(const ConnectionHarness &) = delete;
    std::error_code start(bool tcp = false) {
        if (started_)
            return code(EALREADY);
        started_ = true;
        thread_ = std::thread([this, tcp] {
            run(tcp);
        });
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] {
            return ready_;
        });
        const auto error = startup_error_;
        lock.unlock();
        if (error)
            (void)join();
        return error;
    }
    bool accepting() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return wakeup_ >= 0 && !stop_.load() && !deadline_.load();
    }
    /** @brief 只注入测试socket；拒绝保持源，成功由测试I/O线程直接admit到真实Core。 */
    std::error_code adopt(UniqueFd &source) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!source || wakeup_ < 0 || stop_.load() || deadline_.load())
            return code(ECANCELED);
        if (inputs_.size() == 1024)
            return code(EAGAIN);
        inputs_.push_back(std::move(source));
        return notify_eventfd(wakeup_);
    }
    int wakeup_fd() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return wakeup_;
    }
    std::uint16_t port() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return port_;
    }
    std::variant<CompletionTarget, std::error_code> completion_target() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (wakeup_ < 0 || stop_.load() || released_)
            return code(ECANCELED);
        return CompletionTarget{&completions_, wakeup_};
    }
    std::error_code request_stop() {
        stop_.store(true);
        const std::lock_guard<std::mutex> lock(mutex_);
        return wakeup_ >= 0 ? notify_eventfd(wakeup_) : std::error_code{};
    }
    std::error_code request_drain(std::int64_t deadline) {
        if (deadline <= 0)
            return code(EINVAL);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (wakeup_ < 0 || stop_.load() || released_)
            return code(ECANCELED);
        if (!deadline_.load() || deadline < deadline_.load())
            deadline_.store(deadline);
        return notify_eventfd(wakeup_);
    }
    void release_publishers() {
        stop_.store(true);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_one();
    }
    std::error_code join() {
        if (thread_.joinable())
            thread_.join();
        const auto result = status();
        return result.first_error ? result.first_error : result.cleanup_error;
    }
    std::error_code stop() {
        (void)request_stop();
        release_publishers();
        return join();
    }
    ReactorReport status() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto result = report_;
        result.stop_requested = stop_.load();
        return result;
    }
};
} // namespace siftwing::test
