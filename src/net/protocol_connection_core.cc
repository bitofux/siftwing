/*
 * PROJECT : SIFTWING
 * FILE    : protocol_connection_core.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 所属I/O线程逐轮驱动的连接、协议、身份、期限与Completion处理核心
 */
#include "protocol_connection_core.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include "siftwing/logging/logger.hpp"
#include "siftwing/net/connection_identity.hpp"
#include "siftwing/net/deadline_timer.hpp"
#include <nlohmann/json.hpp>
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
void increment_counter(std::size_t &value) noexcept {
    if (value != std::numeric_limits<std::size_t>::max())
        ++value;
}
void merge_io(metrics::ConnectionIo &into, const metrics::ConnectionIo &from) noexcept {
    into.received_bytes = metrics::add(into.received_bytes, from.received_bytes);
    into.sent_bytes = metrics::add(into.sent_bytes, from.sent_bytes);
    into.input_high_bytes = std::max(into.input_high_bytes, from.input_high_bytes);
    into.output_high_bytes = std::max(into.output_high_bytes, from.output_high_bytes);
}
// 定时基础失败必须传播到Reactor首因，不能被局部客户端异常处理吞掉。
struct DeadlineFailure final : std::system_error {
    explicit DeadlineFailure(std::error_code error) : std::system_error(error) {}
};
DeadlineTime now() {
    const auto value = monotonic_now();
    if (const auto *error = std::get_if<std::error_code>(&value))
        throw DeadlineFailure(*error);
    return std::get<DeadlineTime>(value);
}
} // namespace

/** @brief 复用既有Sessions行为；实现对象只由创建它的I/O线程访问。 */
class ProtocolConnectionCore::Impl final {
    struct Slot {
        std::unique_ptr<Connection> connection;
        ConnectionRegistration registration{}; // 身份与epoll key只由所属I/O线程发布/撤销。
        std::uint32_t mask = 0;
        bool runnable = false, serviced = false;
        bool awaiting_completion = false; // 仅所属线程读写；发布者不接触slot。
        DeadlineTime deadline = 0; // 0为当前状态未启用；每slot最多一个idle或request条目。
        Frame request_metadata{MessageType::ping, 0, 0, "{}"}; // 仅保存受信类型/ID供硬限错误关联。
    };
    std::vector<Slot> slots_;
    int epoll_;
    int timer_; // 外层I/O拥有的timerfd借用，覆盖Core完整生命周期。
    const ProtocolConnectionConfig &config_;
    ReactorReport &report_;
    metrics::ConnectionIo closed_io_; // 关闭前只合并一次，后续采样再加仍活跃连接，避免重复累计。
    const std::atomic<std::int64_t> &drain_requested_; // 控制者只发布期限，连接仍由此线程处理。
    DeadlineTime drain_deadline_ = 0;
    DeadlineQueue deadlines_; // key=slot下标+1；drop真实删除后才允许slot复用，无陈旧条目。
    ConnectionIdentityTable identities_; // 代次跨slot复用保留，token仍全表单调且0保留wakeup。
    void log(logging::Event event,
        logging::Level level,
        ConnectionId identity,
        std::optional<std::uint64_t> request = {},
        logging::Code reason = logging::Code::ok,
        int native_error = 0) noexcept {
        if (!config_.logger)
            return;
        logging::Record record;
        record.level = level;
        record.module = logging::Module::reactor;
        record.event = event;
        record.code = reason;
        record.connection = identity;
        record.request_id = request;
        // Sub事件记录观察时点；某次业务实际使用的版本由worker持有的句柄记录。
        record.index_version = config_.service_info ? std::optional<std::uint64_t>(
                                                          config_.service_info->index_version())
                                                    : config_.log_index_version;
        record.native_error = native_error;
        config_.logger->write(record);
    }
    void log_timeout(const Slot &slot) noexcept {
        const bool pending = slot.connection->request_pending();
        log(pending ? logging::Event::request_expired : logging::Event::idle_expired,
            logging::Level::warn,
            slot.registration.identity,
            pending ? std::optional<std::uint64_t>(slot.request_metadata.request_id) : std::nullopt,
            logging::Code::timeout,
            ETIMEDOUT);
    }
    void drop(Slot &slot,
        std::error_code error = {},
        bool cleanup = false,
        logging::Code reason = logging::Code::ok) noexcept {
        const auto category = reason != logging::Code::ok  ? reason
                              : error.value() == ETIMEDOUT ? logging::Code::timeout
                              : error                      ? logging::Code::io_error
                                                           : logging::Code::ok;
        log(logging::Event::connection_closed,
            category == logging::Code::ok ? logging::Level::info : logging::Level::warn,
            slot.registration.identity,
            slot.connection->request_pending()
                ? std::optional<std::uint64_t>(slot.request_metadata.request_id)
                : std::nullopt,
            category,
            error.value());
        (void)deadlines_.erase(
            static_cast<std::uint64_t>(slot.registration.identity.slot_index) + 1);
        slot.deadline = 0;
        (void)identities_.retire(slot.registration.identity); // 先撤销身份，再关闭可能复用的fd。
        first(report_.first_connection_error, error);
        auto &diagnostic = cleanup ? report_.cleanup_error : report_.first_connection_error;
        if (slot.connection->get() >= 0) {
            first(diagnostic, epoll_remove(epoll_, slot.connection->get()));
            first(diagnostic, slot.connection->close());
        }
        // Connection可自行终止；此时不DEL已失效且可能复用的旧数字。
        merge_io(closed_io_, slot.connection->io_stats());
        slot.connection.reset();
        slot.registration = {};
        slot.mask = 0;
        slot.runnable = false;
        slot.awaiting_completion = false;
        --report_.active;
        increment_counter(report_.dropped);
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
        slot.deadline =
            deadline_after(request ? config_.request_timeout_ms : config_.idle_timeout_ms);
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
            increment_counter(report_.request_timeouts);
        else
            increment_counter(report_.idle_timeouts);
        log_timeout(slot);
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
                    increment_counter(report_.request_timeouts);
                else
                    increment_counter(report_.idle_timeouts);
                log_timeout(slot);
                drop(slot, code(ETIMEDOUT));
            }
        }
    }
    void arm() {
        // 未请求排空且两项都关闭时保持旧disarmed行为，不增加时钟/设时系统调用。
        if (!config_.idle_timeout_ms && !config_.request_timeout_ms && !drain_deadline_)
            return;
        const auto next = deadlines_.next();
        auto when = next ? std::optional<DeadlineTime>(next->when) : std::nullopt;
        if (drain_deadline_ && (!when || drain_deadline_ < *when))
            when = drain_deadline_;
        if (const auto error = arm_deadline_timer(timer_, when))
            throw DeadlineFailure(error);
    }
    /** @brief 保持原关联检查顺序；仅合法响应的硬限拒绝可替换为一次固定错误。 */
    QueueStop queue_response(Slot &slot, const Frame &response) {
        auto &connection = *slot.connection;
        const auto result = connection.queue_response(response).stop;
        if (result != QueueStop::output_limit) {
            if (result == QueueStop::queued)
                report_.response_statuses[response.status] =
                    metrics::add(report_.response_statuses[response.status], 1);
            return result;
        }
        increment_counter(report_.output_limit_rejections);
        log(logging::Event::output_limited,
            logging::Level::warn,
            slot.registration.identity,
            slot.request_metadata.request_id,
            logging::Code::output_limit);
        try {
            const auto fallback = make_error_response(
                slot.request_metadata, ErrorStatus::resource_limit, config_.max_body_bytes);
            const auto *frame = std::get_if<Frame>(&fallback);
            if (frame && connection.queue_response(*frame).stop == QueueStop::queued) {
                auto &counter =
                    report_
                        .response_statuses[static_cast<std::size_t>(ErrorStatus::resource_limit)];
                counter = metrics::add(counter, 1);
                return QueueStop::queued;
            }
        } catch (...) {
            increment_counter(report_.output_limit_closes);
            throw; // 外层统一记录原异常并局部drop；不递归分配第二个错误。
        }
        increment_counter(report_.output_limit_closes);
        return QueueStop::output_limit;
    }
    void service(Slot &slot) {
        observe_drain();
        if (!slot.connection)
            return;
        if (expire(slot))
            return;
        slot.serviced = true;
        slot.runnable = false;
        auto &connection = *slot.connection;
        try {
            if (!connection.request_pending()) {
                if (drain_requested_.load())
                    return; // 请求可能在本轮epoll期间发布；不再启动下一次read。
                auto read =
                    connection.read_request(config_.read_byte_budget, config_.io_attempt_budget);
                if (read.stop == ReadStop::request) {
                    // 完整受信帧在此进入pending，替换idle期限；Body拒绝/PING也使用同一窗口。
                    increment_counter(report_.requests);
                    const auto kind = static_cast<std::size_t>(read.request->message_type);
                    if (kind >= 1 && kind <= report_.request_types.size())
                        report_.request_types[kind - 1] =
                            metrics::add(report_.request_types[kind - 1], 1);
                    if (read.body_error)
                        report_.protocol_errors = metrics::add(report_.protocol_errors, 1);
                    reset_deadline(slot, true);
                    slot.request_metadata.message_type = read.request->message_type;
                    slot.request_metadata.request_id = read.request->request_id;
                    log(logging::Event::request_received,
                        logging::Level::debug,
                        slot.registration.identity,
                        read.request->request_id);
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
                        log(submitted == TaskSubmitResult::accepted ? logging::Event::task_submitted
                                                                    : logging::Event::task_rejected,
                            submitted == TaskSubmitResult::accepted ? logging::Level::debug
                                                                    : logging::Level::warn,
                            slot.registration.identity,
                            metadata.request_id,
                            submitted == TaskSubmitResult::accepted ? logging::Code::ok
                            : submitted == TaskSubmitResult::full   ? logging::Code::queue_full
                            : submitted == TaskSubmitResult::closed ? logging::Code::queue_closed
                                                                    : logging::Code::invalid_task);
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
                        // 单个动态Body内部取同一句柄；旧probe的静态Body路径保持。
                        if (!read.body_error
                            && read.request->message_type == MessageType::server_info_request
                            && (config_.service_info || !config_.server_info_body.empty())
                            && nlohmann::json::parse(read.request->body).empty()) {
                            const auto body = config_.service_info
                                                  ? config_.service_info->server_info()
                                                  : config_.server_info_body;
                            response = make_response(*read.request, body, config_.max_body_bytes);
                            if (std::holds_alternative<ProtocolError>(response))
                                response = make_error_response(*read.request,
                                    ErrorStatus::resource_limit,
                                    config_.max_body_bytes);
                        }
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
                    if (read.stop == ReadStop::header_error || read.stop == ReadStop::truncated)
                        report_.protocol_errors = metrics::add(report_.protocol_errors, 1);
                    drop(slot,
                        read.error,
                        false,
                        read.stop == ReadStop::header_error || read.stop == ReadStop::truncated
                            ? logging::Code::protocol_error
                            : logging::Code::ok);
                    return;
                }
                if (read.bytes && !connection.request_pending())
                    reset_deadline(slot, false);
            }
            // submitter/编码也可消耗时间；开始发送前同时裁决公共排空与原请求期限。
            observe_drain();
            if (!slot.connection)
                return;
            if (expire(slot))
                return;
            if (connection.needs_write()) {
                const auto write =
                    connection.flush_output(config_.write_byte_budget, config_.io_attempt_budget);
                // 非阻塞syscall和有限多次send仍可能跨期限；在宣告排空/finish之前再核同钟。
                if ((write.stop == WriteStop::drained || write.stop == WriteStop::would_block
                        || write.stop == WriteStop::budget_exhausted)
                    && expire(slot))
                    return;
                if (write.stop == WriteStop::drained) {
                    // send可能跨排空截止；先裁决同钟时间，不能把超期结果计为排空成功。
                    observe_drain();
                    if (!slot.connection)
                        return;
                    if (!connection.finish_request()) {
                        drop(slot, code(EIO));
                        return;
                    }
                    increment_counter(report_.responses);
                    if (drain_deadline_) {
                        increment_counter(report_.drain_responses);
                        drop(slot);
                        return; // 不交付已缓冲的后续请求。
                    }
                    reset_deadline(slot, false);
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
    Impl(int epoll,
        int timer,
        const ProtocolConnectionConfig &config,
        ReactorReport &report,
        const std::atomic<std::int64_t> &drain_requested)
        : slots_(config.max_connections), epoll_(epoll), timer_(timer), config_(config),
          report_(report), drain_requested_(drain_requested), deadlines_(config.max_connections),
          identities_(config.reactor_id, config.max_connections) {}
    /** @brief 只由所属I/O线程观察单调排空请求；关闭空闲或截止连接，不取消拥有任务。 */
    void observe_drain() {
        const auto requested = drain_requested_.load();
        if (!requested)
            return;
        const bool beginning = !drain_deadline_;
        drain_deadline_ = requested;
        report_.drain_started = true;
        report_.state = ReactorState::draining;
        const bool expired = now() >= drain_deadline_;
        if (!beginning && !expired)
            return;
        for (auto &slot : slots_)
            if (slot.connection
                && (expired || (beginning && !slot.connection->request_pending()))) {
                if (expired && slot.connection->request_pending())
                    increment_counter(report_.drain_timeouts);
                drop(slot);
            }
    }
    bool drained() {
        observe_drain();
        return drain_deadline_ && !report_.active;
    }
    void admit(UniqueFd &source) {
        auto slot = std::find_if(slots_.begin(), slots_.end(), [](const Slot &value) {
            return !value.connection;
        });
        if (slot == slots_.end()) {
            increment_counter(report_.rejected);
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
        increment_counter(report_.admitted);
        increment_counter(report_.active);
        log(logging::Event::connection_opened, logging::Level::info, slot->registration.identity);
        slot->deadline = idle_deadline;
        if (idle_deadline
            && deadlines_.upsert(
                   {static_cast<std::uint64_t>(slot->registration.identity.slot_index) + 1,
                       idle_deadline})
                   != DeadlineUpdate::stored)
            throw DeadlineFailure(
                code(EIO)); // 活跃数不超容量且新slot无旧条目，此分支仅防不变量破坏。
    }
    bool prepare_round() {
        observe_drain();
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
        return scheduled;
    }
    void check_deadlines() {
        observe_drain();
        expire_due();
        arm();
    }
    void dispatch(std::uint64_t token, std::uint32_t events) {
        const auto identity = identities_.lookup(token);
        if (!identity)
            return; // DEL不撤销已取得的事件；从不按复用fd猜当前连接。
        auto &slot = slots_[identity->slot_index];
        if (slot.connection && slot.registration.identity == *identity && !slot.serviced) {
            if (slot.awaiting_completion && (events & (EPOLLHUP | EPOLLERR)))
                drop(slot, code(ECONNRESET));
            else
                service(slot);
        }
    }

    void complete(const concurrency::RoutedCompletion &result) {
        observe_drain();
        increment_counter(report_.completion_received);
        const auto identity = result.route();
        if (!identities_.matches(identity)) {
            increment_counter(report_.completion_stale);
            log(logging::Event::completion_stale,
                logging::Level::warn,
                identity,
                result.payload()->frame()->request_id,
                logging::Code::stale_identity);
            return; // 先查完整身份，不能按旧slot/fd猜当前Connection。
        }
        auto &slot = slots_[identity.slot_index];
        if (!slot.connection || slot.registration.identity != identity || !slot.awaiting_completion
            || !slot.connection->request_pending()) {
            increment_counter(report_.completion_rejected);
            return;
        }
        try {
            // 即使completion先于timer事件被消费，也不能应用已到期请求。
            if (expire(slot)) {
                increment_counter(report_.completion_rejected);
                return;
            }
            const auto queued = queue_response(slot, *result.payload()->frame());
            if (queued != QueueStop::queued) {
                increment_counter(report_.completion_rejected);
                if (queued == QueueStop::output_limit)
                    drop(slot, code(EMSGSIZE));
                return; // 错ID/类型或重复结果不解除正确请求门禁。
            }
        } catch (const DeadlineFailure &) {
            increment_counter(report_.completion_rejected);
            throw;
        } catch (...) {
            increment_counter(report_.completion_rejected);
            drop(slot, exception_code());
            return;
        }
        slot.awaiting_completion = false;
        increment_counter(report_.completion_applied);
        log(logging::Event::completion_applied,
            logging::Level::debug,
            identity,
            result.payload()->frame()->request_id);
        // 复制已应用；后续I/O/定时设施失败不将同一结果再计为rejected。
        service(slot);
    }

    /** @brief 所属线程发布前复制完整I/O累计和当前缓冲；峰值在Connection变化点记录。 */
    void observe_metrics() noexcept {
        report_.io = closed_io_;
        report_.input_bytes = report_.output_bytes = 0;
        for (const auto &slot : slots_)
            if (slot.connection) {
                merge_io(report_.io, slot.connection->io_stats());
                report_.input_bytes =
                    metrics::add(report_.input_bytes, slot.connection->buffered_bytes());
                report_.output_bytes =
                    metrics::add(report_.output_bytes, slot.connection->output_bytes());
            }
    }
    void close() noexcept {
        for (auto &slot : slots_)
            if (slot.connection)
                drop(slot, {}, true);
    }
};

ProtocolConnectionCore::ProtocolConnectionCore(int epoll,
    int timer,
    const ProtocolConnectionConfig &config,
    ReactorReport &report,
    const std::atomic<std::int64_t> &drain_requested)
    : impl_(std::make_unique<Impl>(epoll, timer, config, report, drain_requested)) {}
ProtocolConnectionCore::~ProtocolConnectionCore() noexcept = default;
bool ProtocolConnectionCore::drained() {
    return impl_->drained();
}
void ProtocolConnectionCore::admit(UniqueFd &source) {
    impl_->admit(source);
}

bool ProtocolConnectionCore::prepare_round() {
    return impl_->prepare_round();
}
void ProtocolConnectionCore::check_deadlines() {
    impl_->check_deadlines();
}
void ProtocolConnectionCore::dispatch(std::uint64_t token, std::uint32_t events) {
    impl_->dispatch(token, events);
}
void ProtocolConnectionCore::complete(const concurrency::RoutedCompletion &result) {
    impl_->complete(result);
}
void ProtocolConnectionCore::observe_metrics() noexcept {
    impl_->observe_metrics();
}
void ProtocolConnectionCore::close() noexcept {
    impl_->close();
}
} // namespace siftwing::net
