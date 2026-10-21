/*
 * PROJECT : SIFTWING
 * FILE    : online_server.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 拥有任务适配、完整结果发布与生产控制者停止屏障
 * IMPLEMENTATION : 调用线程主动驱动全部I/O；停止提交后完整join发布者，再释放通知目标。
 */
#include "siftwing/net/online_server.hpp"
#include "siftwing/net/deadline_timer.hpp"
#include "protocol_connection_core.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include "siftwing/net/signalfd_source.hpp"
#include "siftwing/logging/logger.hpp"
#include <arpa/inet.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <limits>
#include <netinet/tcp.h>

namespace siftwing::net {
namespace {
std::error_code code(int value) noexcept {
    return {value, std::generic_category()};
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
void first(std::atomic<int> &error, int value) noexcept {
    int empty = 0;
    (void)error.compare_exchange_strong(empty, value);
}
/** @brief 仅借用服务、真实完成目标和首因通道；拥有请求，不借用连接。 */
class SearchJob final : public concurrency::WorkerTask {
public:
    std::unique_ptr<concurrency::RoutedSearchTask> task;
    SearchJob(const query::SearchService &service,
        CompletionTarget target,
        std::atomic<int> &error,
        logging::Logger *logger,
        metrics::QueryCounters &metrics)
        : service_(service), target_(target), error_(error), logger_(logger), metrics_(metrics) {}
    void execute() override {
        try {
            // 服务分配/未知异常尝试映射一次固定安全错误；错误构造再失败只记录首因，不递归。
            const bool observe = logger_ && logger_->enabled(logging::Level::debug);
            logging::Record record;
            record.level = logging::Level::debug;
            record.module = logging::Module::worker;
            record.connection = task->route();
            record.request_id = task->payload()->request()->request_id;
            query::CacheObservation cache = query::CacheObservation::not_checked;
            // 服务延迟始终采集，不依赖日志级别；等待队列与发布阶段在此窗口之外。
            const auto begin = std::chrono::steady_clock::now();
            query::SearchServiceResult result = ErrorStatus::internal_error;
            try {
                result = service_.execute(
                    *task->payload(), observe ? &cache : nullptr, &record.index_version);
            } catch (...) {
                result = ErrorStatus::internal_error;
            }
            const auto elapsed =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin)
                                               .count());
            metrics_.record(task->payload()->request()->message_type == MessageType::search_request
                                ? metrics::QueryKind::search
                                : metrics::QueryKind::suggest,
                elapsed,
                std::holds_alternative<std::string>(result));
            if (observe) {
                // 观察只取此调用局部值，不用共享LRU计数差猜命中；不持有任何连接借用。
                switch (cache) {
                    case query::CacheObservation::not_checked:
                        break;
                    case query::CacheObservation::disabled:
                        record.cache_state = logging::CacheState::disabled;
                        break;
                    case query::CacheObservation::miss:
                        record.cache_state = logging::CacheState::miss;
                        break;
                    case query::CacheObservation::hit:
                        record.cache_state = logging::CacheState::hit;
                        break;
                    case query::CacheObservation::unavailable:
                        record.cache_state = logging::CacheState::unavailable;
                        break;
                }
                record.elapsed_us = elapsed;
                record.module = logging::Module::cache;
                record.event = logging::Event::cache_lookup;
                logger_->write(record);
                record.module = logging::Module::worker;
                record.event = logging::Event::query_finished;
                if (const auto *body = std::get_if<std::string>(&result))
                    logger_->write_query(record, *body);
            }
            if (const auto *status = std::get_if<ErrorStatus>(&result); status && logger_) {
                record.level = logging::Level::warn;
                record.event = logging::Event::query_finished;
                record.code = logging::Code::business_error;
                record.native_error = static_cast<int>(*status);
                logger_->write(record);
            }
            auto built =
                std::holds_alternative<std::string>(result)
                    ? concurrency::RoutedCompletion::success(*task, std::get<std::string>(result))
                    : concurrency::RoutedCompletion::error(*task, std::get<ErrorStatus>(result));
            if (!std::holds_alternative<concurrency::RoutedCompletion>(built)) {
                first(error_, EMSGSIZE);
                return;
            }
            auto output = std::make_unique<concurrency::RoutedCompletion>(
                std::get<concurrency::RoutedCompletion>(std::move(built)));
            const auto receipt = try_publish_and_notify(*target_.queue, output, target_.wakeup_fd);
            if (logger_) {
                record.event = receipt.publish == concurrency::CompletionPublishResult::accepted
                                       && !receipt.notification_error
                                   ? logging::Event::completion_published
                                   : logging::Event::completion_rejected;
                record.level = record.event == logging::Event::completion_published
                                   ? logging::Level::debug
                                   : logging::Level::warn;
                record.code = record.event == logging::Event::completion_published
                                  ? logging::Code::ok
                                  : logging::Code::publication_failed;
                record.native_error = receipt.notification_error.value();
                record.elapsed_us.reset();
                record.cache_state = logging::CacheState::not_checked;
                logger_->write(record);
            }
            // stopping期间closed是预期立即丢弃；其他拒绝/通知失败停整体，不重试消费源。
            if (receipt.publish != concurrency::CompletionPublishResult::accepted
                && receipt.publish != concurrency::CompletionPublishResult::closed)
                first(error_, ENOBUFS);
            if (receipt.notification_error)
                first(error_, receipt.notification_error.value());
        } catch (...) {
            first(error_, exception_code().value());
        }
    }

private:
    const query::SearchService &service_;
    CompletionTarget target_;
    std::atomic<int> &error_;
    logging::Logger *logger_;
    metrics::QueryCounters &metrics_; // server独占并覆盖最后worker执行，不持查询锁调用service。
};
/** @brief 唯一I/O提交适配；目标在监听开放前冻结，覆盖最后worker完整发布及通知。 */
class Submitter final : public RoutedRequestSubmitter {
public:
    CompletionTarget target{nullptr, -1};
    concurrency::WorkerPool *pool = nullptr; // start前冻结，生命覆盖最后提交调用。
    Submitter(const query::SearchService &service,
        std::atomic<int> &error,
        logging::Logger *logger,
        metrics::QueryCounters &metrics)
        : service_(service), error_(error), logger_(logger), metrics_(metrics) {}
    concurrency::TaskSubmitResult try_submit(
        std::unique_ptr<concurrency::RoutedSearchTask> &source) override {
        if (!source || !source->valid())
            return concurrency::TaskSubmitResult::invalid_task;
        auto job = std::make_unique<SearchJob>(service_, target, error_, logger_, metrics_);
        // 所有分配先于消费；pool拒绝/初次锁异常恢复原unique_ptr，accepted后不访问可能已销毁job。
        job->task = std::move(source);
        std::unique_ptr<concurrency::WorkerTask> erased = std::move(job);
        try {
            const auto receipt = pool->try_submit(erased);
            if (receipt != concurrency::TaskSubmitResult::accepted)
                source = std::move(static_cast<SearchJob *>(erased.get())->task);
            return receipt;
        } catch (...) {
            source = std::move(static_cast<SearchJob *>(erased.get())->task);
            throw;
        }
    }

private:
    const query::SearchService &service_;
    std::atomic<int> &error_;
    logging::Logger *logger_;
    metrics::QueryCounters &metrics_;
};
void validate(const OnlineServerConfig &config) {
    const auto &l = config.listen;
    const auto &c = config.connection;
    const auto limit = frame_header_bytes + frame_max_body_bytes;
    if (!config.workers || config.workers > 64 || !config.task_capacity
        || config.task_capacity > 1024 || c.request_submitter || c.reactor_id != 0 || c.logger
        || c.log_index_version || c.service_info || !config.reactor.accept_attempt_budget
        || config.reactor.accept_attempt_budget > 1024 || l.backlog <= 0 || !c.max_connections
        || c.max_connections > 65536 || c.max_body_bytes < 2
        || c.max_body_bytes > frame_max_body_bytes || !c.completion_capacity
        || c.completion_capacity > 65536 || !c.completion_budget || c.completion_budget > 1024
        || !c.io_attempt_budget || c.io_attempt_budget > 1024 || !c.read_byte_budget
        || c.read_byte_budget > limit || !c.write_byte_budget || c.write_byte_budget > limit
        || c.idle_timeout_ms < 0 || c.idle_timeout_ms > 86400000 || c.request_timeout_ms < 0
        || c.request_timeout_ms > 86400000 || (l.family != IpFamily::v4 && l.family != IpFamily::v6)
        || l.address.empty() || l.address.size() >= INET6_ADDRSTRLEN
        || l.address.find('\0') != l.address.npos)
        throw std::invalid_argument("invalid server configuration");
    std::array<char, INET6_ADDRSTRLEN> text{};
    std::memcpy(text.data(), l.address.data(), l.address.size());
    std::array<unsigned char, 16> address{};
    if (::inet_pton(l.family == IpFamily::v4 ? AF_INET : AF_INET6, text.data(), address.data())
        != 1)
        throw std::invalid_argument("invalid server address");
    (void)checked_output_watermarks(c.max_body_bytes, c.output);
}
} // namespace
struct OnlineServer::Impl {
    metrics::QueryCounters queries; // 独占收集器声明在pool之前，覆盖其执行及回滚析构。
    std::unique_ptr<query::SearchService> service;
    std::atomic<int> publication_error{0};
    ProtocolConnectionConfig
        connection; // 拥有配置与字符串，覆盖Core完整借用；不保存initialize参数引用。
    OnlineReactorConfig reactor_config;
    OnlineServerReport totals;
    std::atomic<std::int64_t> drain_deadline{0}; // 所属线程设置/观察；兼容Core的稳定期限借用。
    Listener listener;
    UniqueFd epoll, wakeup, timer, termination;
    bool listener_registered = false, wakeup_registered = false, timer_registered = false,
         termination_registered = false;
    std::unique_ptr<concurrency::CompletionQueue> completions;
    std::unique_ptr<Submitter> submitter;
    std::unique_ptr<concurrency::WorkerPool> pool;
    std::unique_ptr<ProtocolConnectionCore> core; // 先销毁借用者；正常/回滚都先经过stop发布者屏障。
    std::size_t worker_count = 0;
    int shutdown_signal = 0; // 创建线程保存首个正常终止信号，不替代故障首因。
    std::chrono::steady_clock::time_point shutdown_begin{};
    std::uint64_t shutdown_us = 0;
    bool shutdown_started = false;
    void begin_shutdown() noexcept {
        if (!shutdown_started) {
            shutdown_begin = std::chrono::steady_clock::now();
            shutdown_started = true;
        }
    }
    explicit Impl(std::unique_ptr<query::SearchService> owned) : service(std::move(owned)) {}
    void record(std::error_code error) {
        if (error && !totals.first_error) {
            totals.first_error = error;
            totals.reactor.first_error = error;
        }
    }
    void cleanup(std::error_code error) noexcept {
        if (error && !totals.reactor.cleanup_error)
            totals.reactor.cleanup_error = error;
    }
    void release_fd(UniqueFd &fd, bool &registered) noexcept {
        if (registered) {
            cleanup(epoll_remove(epoll.get(), fd.get()));
            registered = false;
        }
        cleanup(fd.close());
    }
    void close_listener() noexcept {
        if (listener_registered) {
            cleanup(epoll_remove(epoll.get(), listener.get()));
            listener_registered = false;
        }
        cleanup(listener.close());
    }
    static UniqueFd take(std::variant<UniqueFd, std::error_code> result) {
        if (const auto *error = std::get_if<std::error_code>(&result))
            throw std::system_error(*error);
        return std::move(std::get<UniqueFd>(result));
    }
    void register_control(UniqueFd &fd, bool &registered) {
        if (const auto error = epoll_add(epoll.get(), fd.get(), EPOLLIN, 0))
            throw std::system_error(error);
        registered = true;
    }
    void initialize(const OnlineServerConfig &config) {
        worker_count = config.workers;
        connection = config.connection;
        connection.output = checked_output_watermarks(connection.max_body_bytes, connection.output);
        reactor_config = config.reactor;
        connection.server_info_body = service->server_info();
        connection.service_info = service.get();
        connection.logger = config.logger;
        connection.log_index_version = service->index_version();
        submitter =
            std::make_unique<Submitter>(*service, publication_error, config.logger, queries);
        connection.request_submitter = submitter.get();
        completions =
            std::make_unique<concurrency::CompletionQueue>(0, connection.completion_capacity);
        // signalfd预检和固定网络资源先于worker创建；所有者均稳定存于Impl，不借用栈临时量。
        if (reactor_config.termination_signals)
            termination = take(create_termination_signalfd());
        epoll = take(create_epoll());
        wakeup = take(create_eventfd_wakeup());
        timer = take(create_deadline_timer());
        register_control(wakeup, wakeup_registered);
        register_control(timer, timer_registered);
        if (termination)
            register_control(termination, termination_registered);
        core = std::make_unique<ProtocolConnectionCore>(
            epoll.get(), timer.get(), connection, totals.reactor, drain_deadline);
        pool = std::make_unique<concurrency::WorkerPool>(config.workers, config.task_capacity);
        submitter->pool = pool.get();
        submitter->target = {completions.get(), wakeup.get()};
        auto created = Listener::create(config.listen);
        if (auto *error = std::get_if<std::error_code>(&created))
            throw std::system_error(*error);
        listener = std::move(std::get<Listener>(created));
        if (const auto error = epoll_add(epoll.get(), listener.get(), EPOLLIN, 0))
            throw std::system_error(error);
        listener_registered = true;
        totals.reactor.state = ProtocolSubState::running;
    }
    void controls() {
        const auto notification = drain_eventfd(wakeup.get());
        if (notification.state == EventFdDrainState::failed)
            throw std::system_error(notification.error);
        const auto time = drain_deadline_timer(timer.get());
        if (time.state == TimerDrainState::failed)
            throw std::system_error(time.error);
        if (termination) {
            const auto signals = drain_termination_signalfd(termination.get());
            if (signals.state == SignalFdDrainState::failed)
                throw std::system_error(signals.error);
            if (!shutdown_signal)
                shutdown_signal = signals.signal_number;
        }
    }
    void accept_connections() {
        std::size_t attempts = 0;
        while (attempts < reactor_config.accept_attempt_budget) {
            UniqueFd source;
            const auto batch =
                listener.accept_into(&source, 1, reactor_config.accept_attempt_budget - attempts);
            attempts += batch.attempts;
            if (!source) {
                if (batch.stop == AcceptStop::error)
                    throw std::system_error(batch.error);
                break;
            }
            totals.accepted = metrics::add(totals.accepted, 1);
            if (reactor_config.tcp_no_delay) {
                const int enabled = 1;
                if (::setsockopt(
                        source.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled))) {
                    const auto error = code(errno);
                    totals.rejected = metrics::add(totals.rejected, 1);
                    cleanup(source.close());
                    throw std::system_error(error);
                }
            }
            totals.reactor.received = metrics::add(totals.reactor.received, 1);
            try {
                core->admit(source); // 同线程直接转交；不存在fd队列或Main→Sub通知。
            } catch (...) {
                totals.reactor.rejected = metrics::add(totals.reactor.rejected, 1);
                cleanup(source.close());
                throw;
            }
        }
    }
    void step(int timeout_ms) {
        if (const auto value = publication_error.load())
            throw std::system_error(code(value));
        controls();
        if (shutdown_signal && !shutdown_started)
            return; // 报告首信号，不替应用选择排空期限，也不继续接入。
        const bool scheduled = core->prepare_round();
        if (core->drained())
            return;
        const bool pending = completions->status().size != 0;
        std::array<epoll_event, 64> events{};
        const int count = ::epoll_wait(epoll.get(),
            events.data(),
            static_cast<int>(events.size()),
            scheduled || pending ? 0 : timeout_ms);
        if (count < 0) {
            if (errno == EINTR)
                return;
            throw std::system_error(code(errno));
        }
        if (count > static_cast<int>(events.size()))
            throw std::system_error(code(EIO));
        core->check_deadlines(); // 先按同钟裁决，不由同批结果/I/O排序决定超时胜负。
        bool control_ready = false;
        for (int i = 0; i < count; ++i) {
            const auto &event = events[static_cast<std::size_t>(i)];
            if (event.data.u64 == 0) {
                if (event.events & (EPOLLERR | EPOLLHUP))
                    throw std::system_error(code(EIO));
                control_ready = true;
            }
        }
        controls(); // 控制共享0，与连接的全部非零token严格不交叉。
        if (shutdown_signal && !shutdown_started)
            return;
        for (std::size_t n = 0; n < connection.completion_budget; ++n) {
            auto result = completions->try_pop();
            if (!result)
                break;
            core->complete(*result);
        }
        if (const auto value = publication_error.load())
            throw std::system_error(code(value));
        if (control_ready && listener.get() >= 0 && !shutdown_started)
            accept_connections(); // 其他控制源触发时至多有限探测到EAGAIN。
        for (int i = 0; i < count; ++i) {
            const auto &event = events[static_cast<std::size_t>(i)];
            if (event.data.u64 != 0)
                core->dispatch(event.data.u64, event.events);
        }
        core->check_deadlines(); // 新接纳/结果/发送可能改变最早期限，下一次wait前必须重设timer。
        core->observe_metrics();
    }
    std::error_code stop() {
        if (totals.stopped)
            return totals.first_error;
        begin_shutdown();
        close_listener();
        totals.reactor.state = ProtocolSubState::quiescing;
        totals.reactor.stop_requested = true;
        if (pool)
            pool->request_stop();
        if (completions)
            completions->close(); // 关闭准入不证明发布者静止；对象及eventfd继续存活。
        if (core) {
            core->close();
            core->observe_metrics();
        }
        // 同一线程不再执行step/submit，故无需等待任何Sub；完整join覆盖任务析构和最后通知。
        service->stop_loading();
        if (pool)
            pool->join(); // 任务析构及完整发布/通知都已返回，统计不能代替join。
        if (const auto value = publication_error.load())
            record(code(value));
        if (submitter)
            submitter->target = {nullptr, -1};
        if (completions)
            while (auto result = completions->try_pop())
                totals.reactor.completion_discarded =
                    metrics::add(totals.reactor.completion_discarded, 1);
        core.reset();
        release_fd(termination, termination_registered);
        release_fd(timer, timer_registered);
        release_fd(wakeup, wakeup_registered);
        cleanup(epoll.close());
        record(totals.reactor.cleanup_error);
        totals.reactor.state = ProtocolSubState::stopped;
        totals.stopped = true;
        shutdown_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - shutdown_begin)
                                           .count());
        return totals.first_error;
    }
};
OnlineServer::OnlineServer(std::unique_ptr<Impl> state) noexcept : state_(std::move(state)) {}
OnlineServer::~OnlineServer() noexcept {
    if (state_) {
        try {
            (void)state_->stop();
        } catch (...) {
            std::terminate();
        }
    }
}
std::variant<std::unique_ptr<OnlineServer>, std::error_code> OnlineServer::create(
    std::unique_ptr<query::SearchService> service, OnlineServerConfig config) {
    if (!service)
        return code(EINVAL);
    std::unique_ptr<Impl> state;
    try {
        validate(config);
        state = std::make_unique<Impl>(std::move(service));
        state->initialize(config);
        return std::unique_ptr<OnlineServer>(new OnlineServer(std::move(state)));
    } catch (const std::invalid_argument &) {
        if (state)
            (void)state->stop();
        return code(EINVAL);
    } catch (...) {
        const auto error = exception_code();
        if (state)
            (void)state->stop();
        return error;
    }
}
std::uint16_t OnlineServer::port() const noexcept {
    return state_->listener.port();
}
std::error_code OnlineServer::run_once(int timeout_ms) {
    if (state_->totals.stopped)
        return state_->totals.first_error;
    if (timeout_ms < 0 || timeout_ms > 100)
        return code(EINVAL);
    if (const auto value = state_->publication_error.load())
        state_->record(code(value));
    if (state_->totals.first_error)
        return state_->stop();
    try {
        state_->step(timeout_ms);
    } catch (...) {
        state_->record(exception_code());
        return state_->stop();
    }
    return {};
}
int OnlineServer::shutdown_signal() const noexcept {
    return state_->shutdown_signal;
}
std::error_code OnlineServer::stop() {
    return state_->stop();
}
std::error_code OnlineServer::shutdown(std::int64_t timeout_ms) {
    if (state_->totals.stopped)
        return state_->totals.first_error;
    if (timeout_ms < 0 || timeout_ms > 86400000)
        return code(EINVAL);
    if (const auto value = state_->publication_error.load())
        state_->record(code(value));
    if (state_->totals.first_error)
        return state_->stop();
    const auto time = monotonic_now();
    if (const auto *error = std::get_if<std::error_code>(&time)) {
        state_->record(*error);
        return state_->stop();
    }
    const auto current = std::get<DeadlineTime>(time);
    const auto duration = timeout_ms * 1000000;
    if (current > std::numeric_limits<DeadlineTime>::max() - duration) {
        state_->record(code(EOVERFLOW));
        return state_->stop();
    }
    const auto end = current + duration;
    const auto deadline = end ? end : 1; // 同钟0也合法；timerfd的0专指disarm，不能用作截止点。
    // 唯一I/O线程在此同步推进排空，不能等待已取消的Sub线程替自己工作。
    state_->begin_shutdown();
    state_->pool->request_stop();
    state_->drain_deadline.store(deadline);
    state_->close_listener();
    if (state_->totals.reactor.cleanup_error) {
        state_->record(state_->totals.reactor.cleanup_error);
        return state_->stop();
    }
    try {
        while (!state_->core->drained())
            state_->step(100);
    } catch (...) {
        state_->record(exception_code());
    }
    // 网络排空结束不是所有worker已返回；stop内仍完整join后才撤销目标和通知fd。
    return state_->stop();
}
OnlineServerReport OnlineServer::report() const {
    auto result = state_->totals;
    if (state_->pool)
        result.workers = state_->pool->report();
    if (state_->core) {
        state_->core->observe_metrics();
        result.reactor = state_->totals.reactor;
    }
    if (!result.first_error && state_->publication_error.load())
        result.first_error = code(state_->publication_error.load());
    return result;
}
snapshot::SnapshotLoadSubmit OnlineServer::try_load_snapshot(
    snapshot::SnapshotLoadRequest request) {
    if (state_->totals.stopped || state_->shutdown_started)
        return snapshot::SnapshotLoadSubmit::stopped;
    return state_->service->try_load_candidate(std::move(request));
}
std::optional<snapshot::SnapshotLoadCompletion> OnlineServer::take_loaded_snapshot() {
    return state_->service->take_loaded_candidate();
}
snapshot::SnapshotHandle OnlineServer::current_snapshot() const noexcept {
    return state_->service->current_snapshot();
}
std::variant<snapshot::SnapshotPublishResult, std::error_code> OnlineServer::publish_snapshot(
    snapshot::SnapshotHandle candidate) noexcept {
    if (state_->totals.stopped || state_->shutdown_started)
        return code(ECANCELED);
    return state_->service->publish_candidate(std::move(candidate));
}
metrics::Snapshot OnlineServer::metrics() const {
    metrics::Snapshot out;
    out.connections.accepted = state_->totals.accepted;
    out.connections.handed_off = 0; // 旧schema退役字段；直接接纳从admitted观察，不冒称fd交接。
    out.connections.rejected = state_->totals.rejected;
    {
        if (state_->core)
            state_->core->observe_metrics();
        const auto &report = state_->totals.reactor;
        out.connections.admitted = metrics::add(out.connections.admitted, report.admitted);
        out.connections.rejected = metrics::add(out.connections.rejected, report.rejected);
        out.connections.closed = metrics::add(out.connections.closed, report.dropped);
        out.connections.active = metrics::add(out.connections.active, report.active);
        out.connections.discarded = metrics::add(out.connections.discarded, report.discarded);
        out.network.io.received_bytes =
            metrics::add(out.network.io.received_bytes, report.io.received_bytes);
        out.network.io.sent_bytes = metrics::add(out.network.io.sent_bytes, report.io.sent_bytes);
        out.network.io.input_high_bytes =
            std::max(out.network.io.input_high_bytes, report.io.input_high_bytes);
        out.network.io.output_high_bytes =
            std::max(out.network.io.output_high_bytes, report.io.output_high_bytes);
        out.network.input_bytes = metrics::add(out.network.input_bytes, report.input_bytes);
        out.network.output_bytes = metrics::add(out.network.output_bytes, report.output_bytes);
        out.network.protocol_errors =
            metrics::add(out.network.protocol_errors, report.protocol_errors);
        for (std::size_t i = 0; i < out.requests.by_type.size(); ++i)
            out.requests.by_type[i] =
                metrics::add(out.requests.by_type[i], report.request_types[i]);
        for (std::size_t i = 0; i < out.requests.responses.size(); ++i)
            out.requests.responses[i] =
                metrics::add(out.requests.responses[i], report.response_statuses[i]);
        out.requests.drained = metrics::add(out.requests.drained, report.responses);
        out.queues.handoff = 0; // 不存在生产fd移交队列。
        out.queues.completions = state_->completions ? state_->completions->status().size : 0;
        out.shutdown.drain_timeouts =
            metrics::add(out.shutdown.drain_timeouts, report.drain_timeouts);
        out.shutdown.drain_responses =
            metrics::add(out.shutdown.drain_responses, report.drain_responses);
        out.shutdown.stale_completions =
            metrics::add(out.shutdown.stale_completions, report.completion_stale);
        out.shutdown.discarded_completions =
            metrics::add(out.shutdown.discarded_completions, report.completion_discarded);
    }
    const auto workers = state_->pool->report();
    const auto queue = state_->pool->queue_status();
    out.queues.tasks = queue.size;
    out.queues.task_capacity = queue.capacity;
    out.workers = {state_->worker_count, workers.busy, workers.succeeded, workers.failed};
    out.queries = state_->queries.snapshot();
    const auto cache = state_->service->cache_stats();
    out.cache = {cache.hits, cache.misses, cache.evictions, cache.entries, cache.bytes};
    out.index = state_->service->index_stats();
    out.shutdown.started = state_->shutdown_started;
    out.shutdown.finished = state_->totals.stopped;
    out.shutdown.elapsed_us = state_->shutdown_us;
    return out;
}
} // namespace siftwing::net
