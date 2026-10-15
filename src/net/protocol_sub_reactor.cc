/*
 * PROJECT : SIFTWING
 * FILE    : protocol_sub_reactor.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 固定Connection归属、缓存主动继续与发布者静止后的线程内清理
 */
#include "siftwing/net/protocol_sub_reactor.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include "siftwing/net/connection_identity.hpp"
#include "siftwing/net/deadline_timer.hpp"
#include "siftwing/net/deadline_queue.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

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
    config.output = checked_output_watermarks(config.max_body_bytes, config.output);
    return config;
}
// 定时基础失败必须传播到Sub首因，不能被局部客户端异常处理吞掉。
struct DeadlineFailure final : std::system_error {
    explicit DeadlineFailure(std::error_code error) : std::system_error(error) {}
};
DeadlineTime now() {
    const auto value = monotonic_now();
    if (const auto *error = std::get_if<std::error_code>(&value))
        throw DeadlineFailure(*error);
    return std::get<DeadlineTime>(value);
}
// 仅Sub栈内使用；复用Connection协议合同，监听和线程生命周期由外层分别承担。
class Sessions final {
    struct Slot {
        std::unique_ptr<Connection> connection;
        ConnectionRegistration registration{}; // 身份与epoll key只由所属Sub发布/撤销。
        std::uint32_t mask = 0;
        bool runnable = false, serviced = false;
        bool awaiting_completion = false; // 仅所属线程读写；发布者不接触slot。
        DeadlineTime deadline = 0; // 0为当前状态未启用；每slot最多一个idle或request条目。
        Frame request_metadata{MessageType::ping, 0, 0, "{}"}; // 仅保存受信类型/ID供硬限错误关联。
    };
    std::vector<Slot> slots_;
    int epoll_;
    int timer_; // 所属Sub栈拥有的timerfd借用，覆盖Sessions整个生命周期。
    const ProtocolSubConfig &config_;
    ProtocolSubStatus &report_;
    DeadlineQueue deadlines_; // key=slot下标+1；drop真实删除后才允许slot复用，无陈旧条目。
    ConnectionIdentityTable identities_; // 代次跨slot复用保留，token仍全表单调且0保留wakeup。
    void drop(Slot &slot, std::error_code error = {}, bool cleanup = false) noexcept {
        (void)deadlines_.erase(static_cast<std::uint64_t>(slot.registration.identity.slot_index) + 1);
        slot.deadline = 0;
        (void)identities_.retire(slot.registration.identity); // 先撤销身份，再关闭可能复用的fd。
        first(report_.first_connection_error, error);
        auto &diagnostic = cleanup ? report_.cleanup_error : report_.first_connection_error;
        if (slot.connection->get() >= 0) {
            first(diagnostic, epoll_remove(epoll_, slot.connection->get()));
            first(diagnostic, slot.connection->close());
        }
        // Connection可自行终止；此时不DEL已失效且可能复用的旧数字。
        slot.connection.reset();
        slot.registration = {};
        slot.mask = 0;
        slot.runnable = false;
        slot.awaiting_completion = false;
        --report_.active;
        ++report_.dropped;
    }
    DeadlineTime deadline_after(std::int64_t duration) {
        if (!duration)
            return 0;
        const auto current = now();
        const auto nanos = duration * 1000000; // 构造已将时长限为一天，此乘法不溢出。
        if (current > std::numeric_limits<DeadlineTime>::max() - nanos)
            throw DeadlineFailure(code(EOVERFLOW));
        return current + nanos;
    }
    // 同一slot的空闲与请求计时互斥，容量不超过活跃连接；无请求epoch或任务借用。
    void reset_deadline(Slot &slot, bool request) {
        const auto key = static_cast<std::uint64_t>(slot.registration.identity.slot_index) + 1;
        slot.deadline = deadline_after(request ? config_.request_timeout_ms : config_.idle_timeout_ms);
        if (!slot.deadline) {
            (void)deadlines_.erase(key);
            return;
        }
        if (deadlines_.upsert({key, slot.deadline}) != DeadlineUpdate::stored)
            throw DeadlineFailure(code(EIO));
    }
    bool expire(Slot &slot) {
        if (!slot.deadline || slot.deadline > now())
            return false;
        if (slot.connection->request_pending())
            ++report_.request_timeouts;
        else
            ++report_.idle_timeouts;
        drop(slot, code(ETIMEDOUT));
        return true;
    }
    void expire_due() {
        if (!deadlines_.size())
            return;
        const auto current = now();
        // 一个条目对应一个活跃slot，最多固定容量次；同时间多连接不能按一次read只处理一个。
        for (std::size_t n = 0; n < config_.max_connections; ++n) {
            const auto due = deadlines_.pop_due(current);
            if (!due)
                break;
            auto &slot = slots_[static_cast<std::size_t>(due->key - 1)];
            if (slot.connection) {
                if (slot.connection->request_pending())
                    ++report_.request_timeouts;
                else
                    ++report_.idle_timeouts;
                drop(slot, code(ETIMEDOUT));
            }
        }
    }
    void arm() {
        // 两项都关闭时保持旧disarmed行为，不增加时钟/设时系统调用。
        if (!config_.idle_timeout_ms && !config_.request_timeout_ms)
            return;
        const auto next = deadlines_.next();
        if (const auto error = arm_deadline_timer(timer_, next ? std::optional<DeadlineTime>(next->when)
                                                             : std::nullopt))
            throw DeadlineFailure(error);
    }
    /** @brief 保持原关联检查顺序；仅合法响应的硬限拒绝可替换为一次固定错误。 */
    QueueStop queue_response(Slot &slot, const Frame &response) {
        auto &connection = *slot.connection;
        const auto result = connection.queue_response(response).stop;
        if (result != QueueStop::output_limit)
            return result;
        ++report_.output_limit_rejections;
        try {
            const auto fallback = make_error_response(
                slot.request_metadata, ErrorStatus::resource_limit, config_.max_body_bytes);
            const auto *frame = std::get_if<Frame>(&fallback);
            if (frame && connection.queue_response(*frame).stop == QueueStop::queued)
                return QueueStop::queued;
        } catch (...) {
            ++report_.output_limit_closes;
            throw; // 外层统一记录原异常并局部drop；不递归分配第二个错误。
        }
        ++report_.output_limit_closes;
        return QueueStop::output_limit;
    }
    void service(Slot &slot) {
        if (expire(slot))
            return;
        slot.serviced = true;
        slot.runnable = false;
        auto &connection = *slot.connection;
        try {
            if (!connection.request_pending()) {
                auto read =
                    connection.read_request(config_.read_byte_budget, config_.io_attempt_budget);
                if (read.stop == ReadStop::request) {
                    // 完整受信帧在此进入pending，替换idle期限；Body拒绝/PING也使用同一窗口。
                    ++report_.requests;
                    reset_deadline(slot, true);
                    slot.request_metadata.message_type = read.request->message_type;
                    slot.request_metadata.request_id = read.request->request_id;
                    if (!read.body_error && config_.request_submitter
                        && (read.request->message_type == MessageType::search_request
                            || read.request->message_type == MessageType::suggest_request)) {
                        using namespace concurrency;
                        // 保留受信关联字段供提交拒绝响应使用；源Body交给任务后不再借用。
                        const Frame metadata{
                            read.request->message_type, 0, read.request->request_id, "{}"};
                        auto created = SearchTask::create(*read.request, config_.max_body_bytes);
                        auto *payload = std::get_if<SearchTask>(&created);
                        if (!payload) {
                            drop(slot, code(EPROTO));
                            return;
                        }
                        auto routed =
                            RoutedSearchTask::create(slot.registration.identity, *payload);
                        auto source = std::make_unique<RoutedSearchTask>(
                            std::get<RoutedSearchTask>(std::move(routed)));
                        const auto submitted = config_.request_submitter->try_submit(source);
                        if ((submitted == TaskSubmitResult::accepted) != !source) {
                            drop(
                                slot, code(EPROTO)); // 适配器违反拥有交接合同，不能假定任务未提交。
                            return;
                        }
                        if (submitted == TaskSubmitResult::accepted) {
                            slot.awaiting_completion = true;
                        } else {
                            const auto response = make_error_response(metadata,
                                submitted == TaskSubmitResult::full ? ErrorStatus::overloaded
                                : submitted == TaskSubmitResult::closed
                                    ? ErrorStatus::stopping
                                    : ErrorStatus::internal_error,
                                config_.max_body_bytes);
                            const auto *frame = std::get_if<Frame>(&response);
                            const auto queued =
                                frame ? queue_response(slot, *frame) : QueueStop::protocol_error;
                            if (queued != QueueStop::queued) {
                                drop(slot,
                                    code(queued == QueueStop::output_limit ? EMSGSIZE : EPROTO));
                                return;
                            }
                        }
                    } else {
                        auto response =
                            !read.body_error && read.request->message_type == MessageType::ping
                                ? make_response(*read.request, "{}", config_.max_body_bytes)
                                : make_error_response(*read.request,
                                      read.body_error == ProtocolError::json_depth
                                          ? ErrorStatus::resource_limit
                                          : ErrorStatus::invalid_request,
                                      config_.max_body_bytes);
                        const auto *frame = std::get_if<Frame>(&response);
                        const auto queued =
                            frame ? queue_response(slot, *frame) : QueueStop::protocol_error;
                        if (queued != QueueStop::queued) {
                            drop(slot, code(queued == QueueStop::output_limit ? EMSGSIZE : EPROTO));
                            return;
                        }
                    }
                } else if (read.stop == ReadStop::budget_exhausted)
                    slot.runnable = true;
                else if (read.stop != ReadStop::would_block) {
                    drop(slot, read.error);
                    return;
                }
                if (read.bytes && !connection.request_pending())
                    reset_deadline(slot, false);
            }
            // submitter/编码也可消耗时间；开始发送前再次拒绝已过期请求。
            if (expire(slot))
                return;
            if (connection.needs_write()) {
                const auto write =
                    connection.flush_output(config_.write_byte_budget, config_.io_attempt_budget);
                // 非阻塞syscall和有限多次send仍可能跨期限；在宣告排空/finish之前再核同钟。
                if ((write.stop == WriteStop::drained || write.stop == WriteStop::would_block
                        || write.stop == WriteStop::budget_exhausted) && expire(slot))
                    return;
                if (write.stop == WriteStop::drained) {
                    if (!connection.finish_request()) {
                        drop(slot, code(EIO));
                        return;
                    }
                    reset_deadline(slot, false);
                    ++report_.responses;
                    slot.runnable = connection.buffered_bytes() != 0;
                } else if (write.stop != WriteStop::would_block
                           && write.stop != WriteStop::budget_exhausted) {
                    drop(slot, write.error);
                    return;
                }
            }
            // 输出未空时只订阅OUT，避免半关闭RDHUP造成持续空转；排空后主动服务缓存。
            // 等worker期间移除IN/RDHUP，避免缓存后续请求或半关闭导致LT空转。
            // 全关闭的ERR/HUP仍由内核无条件报告；结果到达后恢复正常读写意向。
            const std::uint32_t mask =
                connection.needs_write() ? EPOLLOUT
                : (slot.awaiting_completion || connection.output_backpressured())
                    ? 0U
                    : (EPOLLIN | EPOLLRDHUP);
            if (mask != slot.mask) {
                if (const auto error = epoll_modify(
                        epoll_, connection.get(), mask, slot.registration.poll_token)) {
                    drop(slot, error);
                    return;
                }
                slot.mask = mask;
            }
        } catch (const DeadlineFailure &) {
            throw;
        } catch (...) {
            drop(slot, exception_code());
        }
    }

public:
    Sessions(int epoll, int timer, const ProtocolSubConfig &config, ProtocolSubStatus &report)
        : slots_(config.max_connections), epoll_(epoll), timer_(timer), config_(config), report_(report),
          deadlines_(config.max_connections), identities_(config.reactor_id, config.max_connections) {}
    void admit(UniqueFd &source) {
        auto slot = std::find_if(slots_.begin(), slots_.end(), [](const Slot &value) {
            return !value.connection;
        });
        if (slot == slots_.end()) {
            ++report_.rejected;
            first(report_.cleanup_error, source.close());
            return;
        }
        // 接纳期限在接管/注册前计算，时钟失败保持源，避免把已admitted误计为rejected。
        const auto idle_deadline = deadline_after(config_.idle_timeout_ms);
        auto connection = std::make_unique<Connection>(
            std::move(source), config_.max_body_bytes, 0, config_.output);
        const auto issued = identities_.activate(static_cast<std::size_t>(slot - slots_.begin()));
        const auto *registration = std::get_if<ConnectionRegistration>(&issued);
        if (!registration)
            throw std::system_error(
                code(std::get<IdentityError>(issued) == IdentityError::generation_exhausted
                             || std::get<IdentityError>(issued) == IdentityError::token_exhausted
                         ? EOVERFLOW
                         : EIO));
        const std::uint32_t mask = EPOLLIN | EPOLLRDHUP;
        if (const auto error =
                epoll_add(epoll_, connection->get(), mask, registration->poll_token)) {
            (void)identities_.retire(
                registration->identity); // ADD失败不留半注册，不回收已消费代次/token。
            first(report_.cleanup_error, connection->close());
            throw std::system_error(error);
        }
        slot->connection = std::move(connection);
        slot->registration = *registration;
        slot->mask = mask;
        slot->runnable = false;
        slot->serviced = false;
        ++report_.admitted;
        ++report_.active;
        slot->deadline = idle_deadline;
        if (idle_deadline && deadlines_.upsert({
                static_cast<std::uint64_t>(slot->registration.identity.slot_index) + 1, idle_deadline})
                != DeadlineUpdate::stored)
            throw DeadlineFailure(code(EIO)); // 活跃数不超容量且新slot无旧条目，此分支仅防不变量破坏。
    }
    void tick(int timeout) {
        expire_due();
        bool scheduled = false;
        for (auto &slot : slots_)
            slot.serviced = false;
        for (auto &slot : slots_)
            if (slot.connection && slot.runnable) {
                service(slot);
                scheduled = true;
            }
        arm();
        std::array<epoll_event, 64> events{};
        const int count = ::epoll_wait(
            epoll_, events.data(), static_cast<int>(events.size()), scheduled ? 0 : timeout);
        if (count < 0) {
            if (errno != EINTR)
                throw std::system_error(code(errno));
            return;
        }
        if (count > static_cast<int>(events.size()))
            throw std::system_error(code(EIO));
        expire_due(); // 先处理到期，再处理同一批I/O；epoll事件顺序不决定超时胜负。
        for (int i = 0; i < count; ++i) {
            const auto token = events[static_cast<std::size_t>(i)].data.u64;
            if (token == 0) {
                if (events[static_cast<std::size_t>(i)].events & (EPOLLERR | EPOLLHUP))
                    throw std::system_error(code(EIO));
                // 两种控制fd共用0，不侵占Connection非零token；陈旧就绪读到EAGAIN正常。
                const auto timer = drain_deadline_timer(timer_);
                if (timer.state == TimerDrainState::failed)
                    throw std::system_error(timer.error);
                continue; // eventfd下轮清通知；实际条目已按同钟时间检查。
            }
            const auto identity = identities_.lookup(token);
            if (!identity)
                continue; // 已取epoll批次不会随DEL清空，陈旧key不能猜当前fd。
            auto &slot = slots_[identity->slot_index];
            if (slot.connection && slot.registration.identity == *identity && !slot.serviced) {
                if (slot.awaiting_completion
                    && (events[static_cast<std::size_t>(i)].events & (EPOLLHUP | EPOLLERR)))
                    drop(slot, code(ECONNRESET));
                else
                    service(slot);
            }
        }
    }
    void complete(const concurrency::RoutedCompletion &result) {
        ++report_.completion_received;
        const auto identity = result.route();
        if (!identities_.matches(identity)) {
            ++report_.completion_stale;
            return; // 先查完整身份，不能按旧slot/fd猜当前Connection。
        }
        auto &slot = slots_[identity.slot_index];
        if (!slot.connection || slot.registration.identity != identity || !slot.awaiting_completion
            || !slot.connection->request_pending()) {
            ++report_.completion_rejected;
            return;
        }
        try {
            // 即使completion先于timer事件被消费，也不能应用已到期请求。
            if (expire(slot)) {
                ++report_.completion_rejected;
                return;
            }
            const auto queued = queue_response(slot, *result.payload()->frame());
            if (queued != QueueStop::queued) {
                ++report_.completion_rejected;
                if (queued == QueueStop::output_limit)
                    drop(slot, code(EMSGSIZE));
                return; // 错ID/类型或重复结果不解除正确请求门禁。
            }
        } catch (const DeadlineFailure &) {
            ++report_.completion_rejected;
            throw;
        } catch (...) {
            ++report_.completion_rejected;
            drop(slot, exception_code());
            return;
        }
        slot.awaiting_completion = false;
        ++report_.completion_applied;
        // 复制已应用；后续I/O/定时设施失败不将同一结果再计为rejected。
        service(slot);
    }

    void close() noexcept {
        for (auto &slot : slots_)
            if (slot.connection)
                drop(slot, {}, true);
    }
};
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
void ProtocolSubReactor::run() noexcept {
    UniqueFd epoll, wakeup, timer;
    std::unique_ptr<Sessions> sessions;
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
        sessions = std::make_unique<Sessions>(epoll.get(), timer.get(), config_, local);
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
        while (!stop_.load() && !queue_.status().closed && !completions_.status().closed) {
            const auto notification_result = drain_eventfd(wakeup.get());
            if (notification_result.state == EventFdDrainState::failed)
                throw std::system_error(notification_result.error);
            for (std::size_t n = 0; n < config_.handoff_budget && !stop_.load(); ++n) {
                auto source = queue_.try_pop();
                if (!source)
                    break;
                ++local.received;
                try {
                    sessions->admit(*source);
                } catch (...) {
                    ++local.rejected;
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
                queue_.status().size || completions_.status().size
                || notification_result.state == EventFdDrainState::budget_exhausted;
            sessions->tick(continue_work ? 0 : stop_poll_interval_ms);
            const std::lock_guard<std::mutex> lock(mutex_);
            report_ = local;
        }
    } catch (...) {
        first(local.first_error, exception_code());
    }
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
        sessions.reset(); // 撤销timer/epoll借用后再关闭所属线程资源。
    }
    // 最终同步设施无法工作时不能悄悄把残留fd留到控制者析构；noexcept收口明确terminate。
    while (auto source = queue_.try_pop()) {
        ++local.discarded;
        first(local.cleanup_error, source->close());
    }
    while (auto result = completions_.try_pop())
        ++local.completion_discarded; // 静止后只销毁，不应用停止期间的结果。
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
