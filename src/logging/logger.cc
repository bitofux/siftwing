/*
 * PROJECT : SIFTWING
 * FILE    : logger.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : spdlog同步锁与固定JSON结构；窄fd sink只承担安全输出
 */
#include "siftwing/logging/logger.hpp"
#include "siftwing/net/unique_fd.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <sys/stat.h>
#include <cstdio>
#include <sys/syscall.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <system_error>
#include <stdexcept>

namespace siftwing::logging {
namespace {
using Json = nlohmann::json;
constexpr std::size_t line_limit = 2048;
[[noreturn]] void file_error(int error = errno) {
    throw std::system_error(error, std::generic_category());
}
net::UniqueFd stderr_target() {
    net::UniqueFd out(::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0));
    if (out.get() < 0)
        file_error();
    return out;
}
/**
 * @brief 固定父目录fd并为原生sink每次open预留全新0600文件；归档命名/轮转仍由spdlog负责
 * @note 目录/文件族由本实例独占管理；不防同一用户其他进程恶意并发替换目录项。
 */
class FileTarget final {
public:
    explicit FileTarget(std::string_view input, std::size_t archives) {
        const std::string path(input);
        if (path.empty() || path.find('\0') != path.npos || path.back() == '/')
            file_error(EINVAL);
        directory_ = net::UniqueFd(
            ::open(path.front() == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!directory_)
            file_error();
        std::size_t begin = path.front() == '/' ? 1 : 0;
        for (;;) {
            const auto end = path.find('/', begin);
            const auto part = path.substr(begin, end == path.npos ? end : end - begin);
            if (part == "..")
                file_error(EINVAL);
            if (end == path.npos) {
                if (part.empty() || part == ".")
                    file_error(EINVAL);
                name_ = part;
                break;
            }
            if (!part.empty() && part != ".") {
                net::UniqueFd next(::openat(directory_.get(),
                    part.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (!next)
                    file_error();
                directory_ = std::move(next);
            }
            begin = end + 1;
        }
        struct stat metadata {};
        if (::fstat(directory_.get(), &metadata) < 0)
            file_error();
        if (metadata.st_uid != ::geteuid() || (metadata.st_mode & 0022) != 0)
            file_error(EACCES);
        // 启动时不认领任何旧文件族；避免原生保留策略删除用户已有的同名归档。
        for (std::size_t i = 0; i <= archives; ++i) {
            const auto name = spdlog::sinks::rotating_file_sink_st::calc_filename(name_, i);
            if (::fstatat(directory_.get(), name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) == 0)
                file_error(EEXIST);
            if (errno != ENOENT)
                file_error();
        }
    }
    std::string filename() const {
        return "/proc/self/fd/" + std::to_string(directory_.get()) + "/" + name_;
    }
    void prepare() {
        // before_open先于spdlog的truncate；轮转失败后若旧当前文件仍在，O_EXCL拒绝回退截断。
        reserved_ = net::UniqueFd(::openat(directory_.get(),
            name_.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            0600));
        if (!reserved_)
            file_error();
        if (::fchmod(reserved_.get(), 0600) < 0 || ::fstat(reserved_.get(), &expected_) < 0)
            file_error();
    }
    void opened(std::FILE *file) {
        const int fd = ::fileno(file);
        struct stat metadata {};
        if (fd < 0 || ::fstat(fd, &metadata) < 0)
            file_error();
        if (!S_ISREG(metadata.st_mode) || metadata.st_dev != expected_.st_dev
            || metadata.st_ino != expected_.st_ino)
            file_error(EINVAL);
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
            file_error();
        // 不留下待析构重刷的stdio缓冲；失败后停用的语义与stderr sink一致。
        if (::setvbuf(file, nullptr, _IONBF, 0) != 0)
            file_error(EIO);
        reserved_ = net::UniqueFd{};
    }

private:
    net::UniqueFd directory_, reserved_;
    std::string name_;
    struct stat expected_ {};
};
spdlog::file_event_handlers file_handlers(const std::shared_ptr<FileTarget> &target) {
    spdlog::file_event_handlers handlers;
    handlers.before_open = [target](const spdlog::filename_t &) {
        target->prepare();
    };
    handlers.after_open = [target](const spdlog::filename_t &, std::FILE *file) {
        target->opened(file);
    };
    return handlers;
}
/** @brief 同一互斥区内复查/发布sticky失败；大小判断、命名、归档移动/淘汰复用spdlog原生sink。 */
class RotatingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    RotatingSink(std::string_view file, Rotation rotation, std::atomic<bool> &failed)
        : target_(std::make_shared<FileTarget>(file, rotation.max_files)),
          output_(target_->filename(),
              rotation.max_bytes,
              rotation.max_files,
              false,
              file_handlers(target_)),
          failed_(failed) {}

private:
    std::shared_ptr<FileTarget> target_; // 固定目录覆盖output及其file_helper完整生命期。
    spdlog::sinks::rotating_file_sink_st output_; // 仅在外层sink锁内访问，不创建第二把锁。
    std::atomic<bool> &failed_;
    void sink_it_(const spdlog::details::log_msg &message) override {
        if (failed_.load(std::memory_order_relaxed))
            return;
        try {
            output_.log(message);
        } catch (...) {
            failed_.store(true, std::memory_order_relaxed);
            throw;
        }
    }
    void flush_() override {} // 文件无stdio缓冲；不提供fsync。
    void set_formatter_(std::unique_ptr<spdlog::formatter> formatter) override {
        output_.set_formatter(std::move(formatter));
    }
};
/** @brief 只屏蔽当前线程SIGPIPE；不改进程handler、不消费调用前已有的pending信号。 */
class PipeGuard final {
public:
    PipeGuard() {
        ::sigemptyset(&one_);
        ::sigaddset(&one_, SIGPIPE);
        const int error = ::pthread_sigmask(SIG_BLOCK, &one_, &old_);
        if (error)
            file_error(error);
        sigset_t pending{};
        if (::sigpending(&pending) < 0) {
            const auto saved = errno;
            (void)::pthread_sigmask(SIG_SETMASK, &old_, nullptr);
            file_error(saved);
        }
        pending_ = ::sigismember(&pending, SIGPIPE) == 1;
    }
    void broken() noexcept {
        if (!pending_) {
            const timespec zero{};
            // EPIPE只消费本次write新产生的线程信号；已有pending保持原处理语义。
            (void)::sigtimedwait(&one_, nullptr, &zero);
        }
    }
    ~PipeGuard() {
        (void)::pthread_sigmask(SIG_SETMASK, &old_, nullptr);
    }

private:
    sigset_t one_{}, old_{};
    bool pending_ = false;
};
/** @brief 复用spdlog sink mutex/formatter；不建立另一套日志调度/缓存或错误重试框架。 */
class FdSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit FdSink(net::UniqueFd fd, std::atomic<bool> &failed) noexcept
        : fd_(std::move(fd)), failed_(failed) {}

private:
    net::UniqueFd fd_;
    std::atomic<bool> &failed_; // Impl在output释放后才销毁此原子量。
    void sink_it_(const spdlog::details::log_msg &message) override {
        if (failed_.load(std::memory_order_relaxed))
            return;
        try {
            PipeGuard signals;
            spdlog::memory_buf_t bytes;
            formatter_->format(message, bytes);
            if (bytes.size() > line_limit)
                throw spdlog::spdlog_ex("bounded log record");
            std::size_t done = 0;
            unsigned interruptions = 0;
            while (done < bytes.size()) {
                const auto n = ::write(fd_.get(), bytes.data() + done, bytes.size() - done);
                if (n < 0 && errno == EINTR && ++interruptions <= 128)
                    continue;
                if (n <= 0) {
                    if (n < 0 && errno == EPIPE)
                        signals.broken();
                    throw spdlog::spdlog_ex("log write failed");
                }
                done += static_cast<std::size_t>(n);
            }
        } catch (...) {
            // 在sink锁内发布失败，排队者不得接着部分末行写下一条。
            failed_.store(true, std::memory_order_relaxed);
            throw;
        }
    }
    void flush_() override {} // 无用户态待刷数据；不提供fsync/断电持久性。
};
const char *level_name(Level level) {
    switch (level) {
        case Level::debug:
            return "debug";
        case Level::info:
            return "info";
        case Level::warn:
            return "warn";
        case Level::error:
            return "error";
        case Level::off:
            return "off";
    }
    throw std::invalid_argument("invalid log level");
}
spdlog::level::level_enum spd_level(Level level) {
    switch (level) {
        case Level::debug:
            return spdlog::level::debug;
        case Level::info:
            return spdlog::level::info;
        case Level::warn:
            return spdlog::level::warn;
        case Level::error:
            return spdlog::level::err;
        case Level::off:
            return spdlog::level::off;
    }
    throw std::invalid_argument("invalid log level");
}
const char *module_name(Module module) {
    switch (module) {
        case Module::server:
            return "server";
        case Module::sub:
            return "sub";
        case Module::worker:
            return "worker";
        case Module::cache:
            return "cache";
    }
    throw std::invalid_argument("invalid log module");
}
const char *event_name(Event event) {
    switch (event) {
#define SIFTWING_LOG_EVENT(name)                                                                   \
    case Event::name:                                                                              \
        return #name
        SIFTWING_LOG_EVENT(server_starting);
        SIFTWING_LOG_EVENT(server_started);
        SIFTWING_LOG_EVENT(server_stopped);
        SIFTWING_LOG_EVENT(server_failed);
        SIFTWING_LOG_EVENT(connection_opened);
        SIFTWING_LOG_EVENT(connection_closed);
        SIFTWING_LOG_EVENT(request_received);
        SIFTWING_LOG_EVENT(task_submitted);
        SIFTWING_LOG_EVENT(task_rejected);
        SIFTWING_LOG_EVENT(cache_lookup);
        SIFTWING_LOG_EVENT(query_finished);
        SIFTWING_LOG_EVENT(completion_published);
        SIFTWING_LOG_EVENT(completion_rejected);
        SIFTWING_LOG_EVENT(completion_applied);
        SIFTWING_LOG_EVENT(completion_stale);
        SIFTWING_LOG_EVENT(output_limited);
        SIFTWING_LOG_EVENT(idle_expired);
        SIFTWING_LOG_EVENT(request_expired);
#undef SIFTWING_LOG_EVENT
    }
    throw std::invalid_argument("invalid log event");
}
const char *code_name(Code code) {
    switch (code) {
#define SIFTWING_LOG_CODE(name)                                                                    \
    case Code::name:                                                                               \
        return #name
        SIFTWING_LOG_CODE(ok);
        SIFTWING_LOG_CODE(service_load);
        SIFTWING_LOG_CODE(server_io);
        SIFTWING_LOG_CODE(local_exception);
        SIFTWING_LOG_CODE(io_error);
        SIFTWING_LOG_CODE(timeout);
        SIFTWING_LOG_CODE(queue_full);
        SIFTWING_LOG_CODE(queue_closed);
        SIFTWING_LOG_CODE(invalid_task);
        SIFTWING_LOG_CODE(business_error);
        SIFTWING_LOG_CODE(stale_identity);
        SIFTWING_LOG_CODE(output_limit);
        SIFTWING_LOG_CODE(publication_failed);
        SIFTWING_LOG_CODE(protocol_error);
#undef SIFTWING_LOG_CODE
    }
    throw std::invalid_argument("invalid log code");
}
Json decimal(std::optional<std::uint64_t> value) {
    return value ? Json(std::to_string(*value)) : Json(nullptr);
}
Json cache_name(CacheState value) {
    switch (value) {
        case CacheState::not_checked:
            return nullptr;
        case CacheState::disabled:
            return "disabled";
        case CacheState::miss:
            return "miss";
        case CacheState::hit:
            return "hit";
        case CacheState::unavailable:
            return "unavailable";
    }
    throw std::invalid_argument("invalid log cache state");
}
Json envelope(const Record &record) {
    const auto time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                          .count();
    const auto id = record.connection;
    Json out{{"schema_version", 1},
        {"time_unix_us", std::to_string(time)},
        {"level", level_name(record.level)},
        {"thread_id", std::to_string(::syscall(SYS_gettid))},
        {"module", module_name(record.module)},
        {"event", event_name(record.event)},
        {"code", code_name(record.code)},
        {"reactor_id",
            id                  ? Json(id->reactor_id)
            : record.reactor_id ? Json(*record.reactor_id)
                                : Json(nullptr)},
        {"connection_id", id ? Json(id->slot_index) : Json(nullptr)},
        {"generation", id ? Json(std::to_string(id->generation)) : Json(nullptr)},
        {"request_id", decimal(record.request_id)},
        {"index_version", decimal(record.index_version)},
        {"elapsed_us", record.elapsed_us ? Json(*record.elapsed_us) : Json(nullptr)},
        {"result_count", record.result_count ? Json(*record.result_count) : Json(nullptr)},
        {"cache_state", cache_name(record.cache_state)},
        {"native_error", record.native_error}};
    return out;
}
} // namespace
struct Logger::Impl {
    Level minimum;
    std::atomic<bool> failed{false};
    std::unique_ptr<spdlog::logger> output;
    explicit Impl(Level level) : minimum(level) {}
};
Logger::Logger(std::unique_ptr<Impl> state) noexcept : state_(std::move(state)) {}
Logger::~Logger() noexcept = default;
base::Result<std::unique_ptr<Logger>> Logger::create(
    Level level, std::string_view file, Rotation rotation) {
    using Output = base::Result<std::unique_ptr<Logger>>;
    if (level < Level::debug || level > Level::off || (level == Level::off && !file.empty())
        || rotation.max_bytes < line_limit || rotation.max_bytes > 1073741824
        || rotation.max_files < 1 || rotation.max_files > 20)
        return Output::failure({"日志级别/目标不相容", "log.argument"});
    try {
        auto impl = std::make_unique<Impl>(level);
        if (level != Level::off) {
            std::shared_ptr<spdlog::sinks::sink> sink;
            if (file.empty())
                sink = std::make_shared<FdSink>(stderr_target(), impl->failed);
            else
                sink = std::make_shared<RotatingSink>(file, rotation, impl->failed);
            impl->output = std::make_unique<spdlog::logger>("siftwing", std::move(sink));
            impl->output->set_pattern("%v");
            impl->output->set_level(spd_level(level));
            // 不注册全局logger。handler只置原子标志，不记录原始错误消息，也不再调用日志。
            impl->output->set_error_handler([state = impl.get()](const std::string &) {
                state->failed.store(true, std::memory_order_relaxed);
            });
        }
        return Output::success(std::unique_ptr<Logger>(new Logger(std::move(impl))));
    } catch (const spdlog::spdlog_ex &) {
        return Output::failure({"日志文件轮转目标无法准入", "log.file"});
    } catch (const std::system_error &) {
        return Output::failure({"日志输出目标无法准入", "log.file"});
    }
}
bool Logger::enabled(Level level) const noexcept {
    return level >= state_->minimum && level < Level::off && !failed();
}
bool Logger::failed() const noexcept {
    return state_->failed.load(std::memory_order_relaxed);
}
void Logger::write(const Record &record) noexcept {
    if (!enabled(record.level))
        return;
    try {
        const auto out = envelope(record);
        const auto text = out.dump();
        if (text.size() + 1 > line_limit)
            throw std::length_error("bounded log record");
        state_->output->log(spd_level(record.level), "{}", text);
    } catch (...) {
        state_->failed.store(true, std::memory_order_relaxed);
    }
}
void Logger::write_metrics(const metrics::Snapshot &snapshot, std::uint64_t sample_id) noexcept {
    if (!enabled(Level::info))
        return;
    try {
        Record record;
        record.index_version = snapshot.index.version;
        const auto number = [](std::uint64_t value) {
            return std::to_string(value);
        };
        const auto array = [&number](const auto &values) {
            Json result = Json::array();
            for (const auto value : values)
                result.push_back(number(value));
            return result;
        };
        // 每族单独限长/写入，复用原sink锁与失败隔离；无多行事务或全局采样锁。
        const auto emit = [&](const char *family, Json values) {
            if (!enabled(Level::info))
                return false;
            auto out = envelope(record);
            out["event"] = "metrics_snapshot";
            out["metrics_schema_version"] = 1;
            out["sample_id"] = number(sample_id);
            out["metric_family"] = family;
            out["values"] = std::move(values);
            const auto text = out.dump();
            if (text.size() + 1 > line_limit)
                throw std::length_error("bounded metrics record");
            state_->output->log(spd_level(Level::info), "{}", text);
            return !failed();
        };
        const auto &c = snapshot.connections;
        if (!emit("connections",
                {{"accepted", number(c.accepted)},
                    {"handed_off", number(c.handed_off)},
                    {"rejected", number(c.rejected)},
                    {"admitted", number(c.admitted)},
                    {"closed", number(c.closed)},
                    {"active", number(c.active)},
                    {"discarded", number(c.discarded)}}))
            return;
        const auto &n = snapshot.network;
        if (!emit("network",
                {{"received_bytes", number(n.io.received_bytes)},
                    {"sent_bytes", number(n.io.sent_bytes)},
                    {"input_high_bytes", number(n.io.input_high_bytes)},
                    {"output_high_bytes", number(n.io.output_high_bytes)},
                    {"input_bytes", number(n.input_bytes)},
                    {"output_bytes", number(n.output_bytes)},
                    {"protocol_errors", number(n.protocol_errors)}}))
            return;
        if (!emit("requests",
                {{"by_type", array(snapshot.requests.by_type)},
                    {"responses", array(snapshot.requests.responses)},
                    {"drained", number(snapshot.requests.drained)}}))
            return;
        const auto &q = snapshot.queues;
        if (!emit("queues",
                {{"tasks", number(q.tasks)},
                    {"task_capacity", number(q.task_capacity)},
                    {"handoff", number(q.handoff)},
                    {"completions", number(q.completions)}}))
            return;
        const auto &w = snapshot.workers;
        if (!emit("workers",
                {{"count", number(w.count)},
                    {"busy", number(w.busy)},
                    {"succeeded", number(w.succeeded)},
                    {"failed", number(w.failed)}}))
            return;
        const auto latency = [&](const metrics::Latency &value) {
            return Json{{"count", number(value.count)},
                {"failures", number(value.failures)},
                {"sum_us", number(value.sum_us)},
                {"max_us", number(value.max_us)},
                {"buckets", array(value.buckets)},
                {"collection_failed", snapshot.queries.collection_failed}};
        };
        if (!emit("search", latency(snapshot.queries.search)))
            return;
        if (!emit("suggest", latency(snapshot.queries.suggest)))
            return;
        const auto &cache = snapshot.cache;
        if (!emit("cache",
                {{"hits", number(cache.hits)},
                    {"misses", number(cache.misses)},
                    {"evictions", number(cache.evictions)},
                    {"entries", number(cache.entries)},
                    {"bytes", number(cache.bytes)}}))
            return;
        if (!emit("index",
                {{"documents", number(snapshot.index.documents)},
                    {"terms", number(snapshot.index.terms)},
                    {"recommendation_terms", number(snapshot.index.recommendation_terms)}}))
            return;
        const auto &stop = snapshot.shutdown;
        (void)emit("shutdown",
            {{"started", stop.started},
                {"finished", stop.finished},
                {"elapsed_us", number(stop.elapsed_us)},
                {"drain_timeouts", number(stop.drain_timeouts)},
                {"drain_responses", number(stop.drain_responses)},
                {"stale_completions", number(stop.stale_completions)},
                {"discarded_completions", number(stop.discarded_completions)}});
    } catch (...) {
        state_->failed.store(true, std::memory_order_relaxed);
    }
}
void Logger::write_query(Record record, std::string_view body) noexcept {
    if (!enabled(record.level))
        return;
    try {
        if (body.size() < 2 || body.size() > 1048576)
            throw std::length_error("bounded result body");
        const auto value = Json::parse(body, [](int depth, Json::parse_event_t, Json &) {
            if (depth > 64)
                throw std::length_error("bounded result depth");
            return true;
        });
        if (!value.is_object() || !value.contains("results") || !value["results"].is_array())
            throw std::invalid_argument("invalid result metadata");
        record.result_count = value["results"].size();
        write(record);
    } catch (...) {
        state_->failed.store(true, std::memory_order_relaxed);
    }
}
} // namespace siftwing::logging
