/*
 * PROJECT : SIFTWING
 * FILE    : online_server.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 拥有任务适配、完整结果发布与生产控制者停止屏障
 * IMPLEMENTATION : Main关闭后等待Sub提交静止，再join worker；完整通知返回后才release。
 */
#include "siftwing/net/online_server.hpp"
#include "siftwing/net/deadline_timer.hpp"
#include "siftwing/logging/logger.hpp"
#include <arpa/inet.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <limits>

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
            record.index_version = service_.index_version();
            query::CacheObservation cache = query::CacheObservation::not_checked;
            // 服务延迟始终采集，不依赖日志级别；等待队列与发布阶段在此窗口之外。
            const auto begin = std::chrono::steady_clock::now();
            query::SearchServiceResult result = ErrorStatus::internal_error;
            try {
                result = service_.execute(*task->payload(), observe ? &cache : nullptr);
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
/** @brief 每Sub独立适配；目标在监听开放前冻结，直到该Sub及worker都join。 */
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
    if (!config.sub_count || config.sub_count > 64 || !config.workers || config.workers > 64
        || !config.task_capacity || config.task_capacity > 1024 || config.sub.request_submitter
        || config.sub.reactor_id != 0 || config.sub.logger || config.sub.log_index_version
        || !config.main.accept_attempt_budget || config.main.accept_attempt_budget > 1024
        || l.backlog <= 0 || (l.family != IpFamily::v4 && l.family != IpFamily::v6)
        || l.address.empty() || l.address.size() >= INET6_ADDRSTRLEN
        || l.address.find('\0') != l.address.npos)
        throw std::invalid_argument("invalid server configuration");
    std::array<char, INET6_ADDRSTRLEN> text{};
    std::memcpy(text.data(), l.address.data(), l.address.size());
    std::array<unsigned char, 16> address{};
    if (::inet_pton(l.family == IpFamily::v4 ? AF_INET : AF_INET6, text.data(), address.data())
        != 1)
        throw std::invalid_argument("invalid server address");
}
} // namespace
struct OnlineServer::Impl {
    metrics::QueryCounters queries; // 独占收集器声明在pool之前，覆盖其执行及回滚析构。
    std::unique_ptr<query::SearchService> service;
    std::unique_ptr<concurrency::WorkerPool> pool;
    std::vector<std::unique_ptr<Submitter>> submitters;
    std::vector<std::unique_ptr<ProtocolSubReactor>> subs;
    std::unique_ptr<MainReactor> main;
    std::atomic<int> publication_error{0};
    OnlineServerReport totals;
    std::size_t worker_count = 0;
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
        if (!totals.first_error)
            totals.first_error = error;
    }
    void initialize(const OnlineServerConfig &config) {
        worker_count = config.workers;
        for (std::size_t i = 0; i < config.sub_count; ++i) {
            auto sink =
                std::make_unique<Submitter>(*service, publication_error, config.logger, queries);
            auto selected = config.sub;
            selected.server_info_body = service->server_info();
            selected.logger = config.logger;
            selected.log_index_version = service->index_version();
            selected.reactor_id = static_cast<std::uint32_t>(i);
            selected.request_submitter = sink.get();
            auto sub = std::make_unique<ProtocolSubReactor>(selected);
            submitters.push_back(std::move(sink));
            subs.push_back(std::move(sub));
        }
        // 全部Sub配置及预分配成功后才创建worker；未启动对象无发布借用。
        pool = std::make_unique<concurrency::WorkerPool>(config.workers, config.task_capacity);
        for (auto &sink : submitters)
            sink->pool = pool.get();
        std::vector<MainReactorTarget> targets;
        for (std::size_t i = 0; i < subs.size(); ++i) {
            if (const auto error = subs[i]->start())
                throw std::system_error(error);
            submitters[i]->target = std::get<CompletionTarget>(subs[i]->completion_target());
            targets.push_back(std::get<MainReactorTarget>(subs[i]->target()));
        }
        auto created = MainReactor::create(targets, config.listen, config.main);
        if (auto *error = std::get_if<std::error_code>(&created))
            throw std::system_error(*error);
        main = std::move(std::get<std::unique_ptr<MainReactor>>(created));
    }
    std::error_code stop() {
        if (totals.stopped)
            return totals.first_error;
        begin_shutdown();
        if (main) {
            record(main->close());
            main.reset();
        }
        for (auto &sub : subs)
            record(sub->request_stop());
        for (auto &sub : subs) {
            // quiescing在调用submitter返回后报告，queue随后关闭；这里只证明不再生产任务。
            for (;;) {
                const auto state = sub->status().state;
                if (state != ProtocolSubState::starting && state != ProtocolSubState::running
                    && state != ProtocolSubState::draining)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (pool)
            pool->join(); // 任务析构及完整发布/通知都已返回，统计不能代替join。
        if (const auto value = publication_error.load())
            record(code(value));
        for (auto &sink : submitters)
            sink->target = {nullptr, -1};
        for (auto &sub : subs)
            sub->release_publishers();
        for (auto &sub : subs)
            record(sub->join());
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
    return state_->main ? state_->main->port() : 0;
}
std::error_code OnlineServer::run_once(int timeout_ms) {
    if (state_->totals.stopped)
        return state_->totals.first_error;
    if (timeout_ms < 0 || timeout_ms > 100)
        return code(EINVAL);
    if (const auto value = state_->publication_error.load())
        state_->record(code(value));
    for (const auto &sub : state_->subs)
        state_->record(sub->status().first_error);
    if (state_->totals.first_error)
        return state_->stop();
    const auto step = state_->main->run_once(timeout_ms);
    state_->totals.accepted = metrics::add(state_->totals.accepted, step.accepted);
    state_->totals.handed_off = metrics::add(state_->totals.handed_off, step.handed_off);
    state_->totals.rejected = metrics::add(state_->totals.rejected, step.rejected);
    state_->record(step.cleanup_error);
    if (step.stop == MainReactorStop::failed || step.stop == MainReactorStop::closed) {
        state_->record(step.error);
        return state_->stop();
    }
    return {};
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
    for (const auto &sub : state_->subs)
        state_->record(sub->status().first_error);
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
    // Main只由本控制线程调度，进入此同步流程后不再accept。先关任务准入；即使Sub正处于
    // 最后一个提交回调，也只能得到closed。先发布排空再Main close，避免handoff关闭抢先终止Sub。
    state_->begin_shutdown();
    state_->pool->request_stop();
    for (auto &sub : state_->subs) {
        const auto error = sub->request_drain(deadline);
        state_->record(sub->status().first_error);
        state_->record(error);
    }
    if (state_->main) {
        state_->record(state_->main->close());
        state_->main.reset();
    }
    for (;;) {
        bool quiescent = true;
        if (const auto value = state_->publication_error.load())
            state_->record(code(value));
        for (const auto &sub : state_->subs) {
            const auto report = sub->status();
            state_->record(report.first_error);
            if (report.state == ProtocolSubState::starting
                || report.state == ProtocolSubState::running
                || report.state == ProtocolSubState::draining)
                quiescent = false;
        }
        if (state_->totals.first_error)
            return state_->stop(); // 故障不继续等待正常排空，首因和旧完整发布屏障保持。
        if (quiescent)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Sub已停止提交/应用，不再等待worker结果；worker仍执行所有accepted任务并完成最后通知。
    // stop的join→撤借用→release→Sub join顺序不变，网络deadline不能缩短借用对象生命期。
    return state_->stop();
}
OnlineServerReport OnlineServer::report() const {
    auto result = state_->totals;
    if (state_->pool)
        result.workers = state_->pool->report();
    for (const auto &sub : state_->subs)
        result.subs.push_back(sub->status());
    if (!result.first_error && state_->publication_error.load())
        result.first_error = code(state_->publication_error.load());
    return result;
}
metrics::Snapshot OnlineServer::metrics() const {
    metrics::Snapshot out;
    out.connections.accepted = state_->totals.accepted;
    out.connections.handed_off = state_->totals.handed_off;
    out.connections.rejected = state_->totals.rejected;
    for (const auto &sub : state_->subs) {
        const auto report = sub->status();
        const auto depths = sub->queue_depths();
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
        out.queues.handoff = metrics::add(out.queues.handoff, depths.handoff);
        out.queues.completions = metrics::add(out.queues.completions, depths.completions);
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
