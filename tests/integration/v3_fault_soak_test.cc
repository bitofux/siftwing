/*
 * PROJECT : SIFTWING
 * FILE    : v3_fault_soak_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 连续加载失败、进程级fd耗尽恢复及真实TCP版本切换的有界持续验证
 * IMPLEMENTATION : 独立快照与黄金帧作为参考；不用生产hook，不把运行时长当性能数据。
 */
#include "siftwing/net/online_server.hpp"
#include "siftwing/net/signalfd_source.hpp"
#include "../support/online_server_fixture.hpp"
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sys/resource.h>
#include <sys/syscall.h>

namespace {
using namespace siftwing;
using namespace siftwing::test;
using Json = nlohmann::json;
using Submit = snapshot::SnapshotLoadSubmit;
unsigned checks = 0;
void expect(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void check(bool ok, const char *why) {
    ++checks; // 仅控制线程计数；客户端线程用独立异常槽与原子计数汇报。
    expect(ok, why);
}
base::SnapshotVersion version(std::uint64_t n) {
    return base::SnapshotVersion::from_integer(n).value();
}
snapshot::SnapshotReadLimits limits() {
    constexpr std::uint64_t cap = 1ULL << 26U;
    return {{{cap, cap}, 1000000, cap, cap},
        {cap, cap, cap},
        {{1000000, cap, cap, 1000000, cap}, 1000000, 1000000, 1000000, 1000000},
        {{1000000, cap, cap, 1000000, cap}, 1000000, 1000000, 1000000, 1000000}};
}
std::size_t threads() {
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator("/proc/self/task"),
            std::filesystem::directory_iterator{}));
}
std::unique_ptr<net::OnlineServer> server(const std::string &root, std::size_t capacity_scale) {
    query::SearchServiceConfig cfg;
    cfg.parent = root + "/valid";
    cfg.version = 100;
    cfg.cache.capacity = 16;
    auto service = query::SearchService::create(cfg);
    if (!service)
        throw std::runtime_error(service.error().context);
    net::OnlineServerConfig config;
    config.connection.max_connections = capacity_scale * 128;
    config.connection.completion_capacity = capacity_scale * 128;
    config.workers = 2;
    config.task_capacity = 8;
    config.reactor.termination_signals = true;
    auto created = net::OnlineServer::create(std::move(service).value(), config);
    if (auto *error = std::get_if<std::error_code>(&created))
        throw std::system_error(*error);
    return std::move(std::get<std::unique_ptr<net::OnlineServer>>(created));
}
net::UniqueFd attach(net::OnlineServer &s, std::size_t total) {
    auto c = client(s.port());
    await([&] {
        if (auto e = s.run_once(0))
            throw std::system_error(e);
        return s.report().reactor.admitted == total;
    });
    return c;
}
Json response(int fd, std::uint16_t type, std::uint64_t id) {
    const auto h = receive(fd, 24);
    std::size_t n = 0;
    for (std::size_t i = 12; i < 16; ++i)
        n = n * 256 + static_cast<unsigned char>(h[i]);
    expect(n <= net::frame_max_body_bytes, "response body budget");
    const auto body = receive(fd, n);
    expect(h + body == wire(type, id, body), "independent type/id/status/frame bytes");
    return Json::parse(body);
}
// 期望值只依赖Python原创样本的标签/文档身份，不向生产Reader询问正确答案。
void consistent(const Json &j, bool suggest) {
    const auto label = j.at("build_version").get<std::string>();
    expect(label == "old" || label == "new" || label == "alias", "known build identity");
    expect(j.at("snapshot_version") == (label == "new" ? "101" : "100"), "version/build pair");
    const auto &hits = j.at("results");
    expect(hits.size() == 1, "independent one shared hit");
    if (suggest)
        expect(hits[0].at("term") == "shared-" + label, "recommendation/build pair");
    else {
        expect(hits[0].at("title") == label, "document/build pair");
        expect(hits[0].at("document_id")
                   == (label == "old"   ? "11"
                       : label == "new" ? "22"
                                        : "33"),
            "document identity/build pair");
    }
}
void query(int fd, std::uint64_t id, bool suggest) {
    send_all(
        fd, wire(suggest ? 4 : 3, id, suggest ? R"({"key":"shared"})" : R"({"query":"shared"})"));
    consistent(response(fd, suggest ? 0x8004 : 0x8003, id), suggest);
}
snapshot::SnapshotLoadCompletion load(
    net::OnlineServer &s, const std::string &parent, std::uint64_t n) {
    check(s.try_load_snapshot({parent, version(n), limits()}) == Submit::accepted, "load accepted");
    // 完成未取也占槽，所以不依赖后台执行速度来判定busy。
    check(s.try_load_snapshot({parent, version(n), limits()}) == Submit::busy, "one occupied slot");
    std::optional<snapshot::SnapshotLoadCompletion> done;
    await([&] {
        done = s.take_loaded_snapshot();
        return bool(done);
    });
    check(done->requested_version == n && done->report && !done->exception, "ordinary completion");
    return std::move(*done);
}
void publish(net::OnlineServer &s, const snapshot::SnapshotLoadCompletion &done) {
    check(done.report->outcome.has_value(), "valid candidate");
    const auto published = s.publish_snapshot(done.report->outcome.value());
    check(std::holds_alternative<snapshot::SnapshotPublishResult>(published)
              && std::get<snapshot::SnapshotPublishResult>(published)
                     == snapshot::SnapshotPublishResult::published,
        "explicit publication");
}
void stopped(const net::OnlineServer &s) {
    const auto report = s.report();
    check(report.stopped && !report.first_error && report.workers.failed == 0, "clean stop report");
    for (const auto &sub : {report.reactor})
        check(sub.state == net::ReactorState::stopped && sub.active == 0,
            "no active Reactor connection");
}
// 只降低当前隔离子进程的软限；异常先关填充fd再恢复原限，不改变硬限/主机配置。
class FdPressure {
    rlimit previous_{};
    std::vector<net::UniqueFd> held_;
    bool changed_ = false;

public:
    FdPressure() {
        held_.reserve(128);
        if (::getrlimit(RLIMIT_NOFILE, &previous_))
            throw std::system_error(errno, std::generic_category());
        auto low = previous_;
        low.rlim_cur = std::min<rlim_t>(previous_.rlim_cur, 64);
        if (::setrlimit(RLIMIT_NOFILE, &low))
            throw std::system_error(errno, std::generic_category());
        changed_ = true;
    }
    void fill() {
        for (;;) {
            const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                check(errno == EMFILE && !held_.empty(), "real process fd exhaustion");
                return;
            }
            held_.emplace_back(fd);
        }
    }
    ~FdPressure() {
        held_.clear();
        if (changed_ && ::setrlimit(RLIMIT_NOFILE, &previous_))
            std::terminate();
    }
};
Json faults(const std::string &root) {
    auto s = server(root, 2);
    siftwing::test::OnlineServerDriver drive(*s);
    auto c = attach(*s, 1);
    (void)load(*s, root + "/valid", 101); // 先建立Loader，耗尽窗口只验证文件打开失败。
    // 先取得独立错误文本，让消毒器在fd充足时完成标准库类型检查；耗尽时仍走Reader原路径。
    const auto emfile_message = std::error_code(EMFILE, std::generic_category()).message();
    const auto baseline = fd_count();
    const auto old = s->current_snapshot();
    std::uint64_t id = 1;
    for (unsigned round = 0; round < 8; ++round) {
        for (const auto *bad : {"incomplete", "sha", "semantic"}) {
            const auto done = load(*s, root + "/" + bad, 101);
            check(!done.report->outcome && !done.report->outcome.error().context.empty(),
                "bad candidate rejected");
            check(s->current_snapshot() == old, "failure preserves exact current handle");
            query(c.get(), id++, false);
            check(fd_count() == baseline, "failed input does not leak fd");
        }
        {
            FdPressure pressure;
            pressure.fill();
            const auto done = load(*s, root + "/valid", 101);
            check(!done.report->outcome && done.report->outcome.error().context == "reader.open"
                      && done.report->outcome.error().message == emfile_message,
                "EMFILE reaches original Reader diagnostic");
            check(s->current_snapshot() == old, "fd pressure keeps current");
        }
        query(c.get(), id++, false);
        publish(*s, load(*s, root + "/valid", 101));
        query(c.get(), id++, true);
        check(std::get<snapshot::SnapshotPublishResult>(s->publish_snapshot(old))
                  == snapshot::SnapshotPublishResult::published,
            "rollback after resource recovery");
        check(fd_count() == baseline, "fd restored after exhaustion and retry");
    }
    check(!s->shutdown(1000), "resource suite shutdown");
    stopped(*s);
    return {{"bad_loads", 24}, {"real_emfile", 8}, {"successful_retries", 8}, {"queries", id - 1}};
}
struct Readers {
    std::atomic<bool> finish{false};
    std::atomic<std::size_t> done{0};
    std::atomic<std::uint64_t> replies{0};
    std::array<std::atomic<bool>, 2> ready{};
    std::array<std::exception_ptr, 2> errors{};
    std::vector<std::thread> jobs;
    void join() {
        finish = true;
        // 客户端退出前仍可能等一个在途响应；拥有线程须继续推进唯一I/O循环再join。
        while (done.load() != jobs.size()) {
            if (io_progress)
                io_progress();
            std::this_thread::yield();
        }
        for (auto &job : jobs)
            if (job.joinable())
                job.join();
    }
    ~Readers() {
        join();
    }
};
Json soak(const std::string &root, std::size_t capacity_scale, unsigned seconds) {
    auto s = server(root, capacity_scale);
    siftwing::test::OnlineServerDriver drive(*s);
    auto a = attach(*s, 1), b = attach(*s, 2);
    (void)load(*s, root + "/valid", 101);
    Readers readers; // 必须在server/client之前析构，异常也先结束客户端再销毁服务。
    for (unsigned i = 0; i < 2; ++i)
        readers.jobs.emplace_back([&, i] {
            try {
                std::uint64_t id = 1;
                do {
                    query(i ? b.get() : a.get(), id++, i != 0);
                    ++readers.replies;
                    readers.ready[i] = true;
                } while (!readers.finish.load());
            } catch (...) {
                readers.errors[i] = std::current_exception();
                readers.finish = true;
            }
            ++readers.done;
        });
    // 两条流都完成首个完整响应后再取运行基线，不把线程构造返回当作业务已就绪。
    await([&] {
        check(!readers.finish.load(), "clients start without failure");
        return readers.ready[0].load() && readers.ready[1].load();
    });
    const auto running_fds = fd_count(), running_threads = threads();
    Json resource_observations = Json::array();
    std::array<std::weak_ptr<const snapshot::SnapshotReader>, 64> retired;
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + std::chrono::seconds(seconds);
    unsigned cycles = 0;
    do {
        auto previous = s->current_snapshot();
        retired[cycles % retired.size()] = previous;
        const auto bad = load(*s, root + "/" + (cycles % 2 ? "sha" : "semantic"), 101);
        check(!bad.report->outcome && s->current_snapshot() == previous,
            "soak failure keeps online view");
        const auto selector = cycles % 3;
        publish(
            *s, load(*s, root + (selector == 1 ? "/alias" : "/valid"), selector == 0 ? 101 : 100));
        ++cycles;
        check(!readers.finish.load(), "both independent clients remain live");
        if (cycles % 32 == 0) {
            const auto observed_fds = fd_count(), observed_threads = threads();
            resource_observations.push_back(
                {{"cycle", cycles}, {"fds", observed_fds}, {"threads", observed_threads}});
            // 此处停止继续发布，允许已在途的旧请求释放Reader；有限等待不放宽精确数量。
            await([&] {
                return fd_count() == running_fds && threads() == running_threads;
            });
            check(fd_count() == running_fds && threads() == running_threads,
                "live fd/thread counts remain bounded");
        }
    } while (cycles < 16 || std::chrono::steady_clock::now() < until);
    readers.join();
    for (const auto &error : readers.errors)
        if (error)
            std::rethrow_exception(error);
    check(readers.replies >= 2, "both traffic streams made progress");
    // 最后一次已接受加载可能仍执行，也可能已完成；确定执行中的关闭另由门禁race测试证明。
    check(s->try_load_snapshot({root + "/incomplete", version(101), limits()}) == Submit::accepted,
        "accepted failure before termination");
    const int signo = capacity_scale == 1 ? SIGINT : SIGTERM;
    check(::kill(::getpid(), signo) == 0 && !s->run_once(100) && s->shutdown_signal() == signo,
        "normal termination observed by Reactor");
    check(!s->shutdown(1000), "soak signal shutdown");
    stopped(*s);
    auto done = s->take_loaded_snapshot();
    check(done && done->report && !done->report->outcome, "accepted failure survives shutdown");
    check(s->try_load_snapshot({root + "/valid", version(101), limits()}) == Submit::stopped,
        "shutdown permanently rejects new load");
    const auto report = s->report();
    check(report.workers.succeeded == readers.replies.load(),
        "every validated request executed once");
    drive.release();
    s.reset();
    for (const auto &weak : retired)
        check(weak.expired(), "sampled retired Reader reclaimed");
    rusage usage{};
    check(::getrusage(RUSAGE_SELF, &usage) == 0, "resource observation");
    return {{"io_reactors", 1},
        {"capacity_scale", capacity_scale},
        {"cycles", cycles},
        {"valid_responses", readers.replies.load()},
        {"elapsed_ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count()},
        {"steady_fds", running_fds},
        {"steady_threads", running_threads},
        {"resource_observations", std::move(resource_observations)},
        {"maxrss_kib", usage.ru_maxrss},
        {"termination_signal", signo}};
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 5)
        return 2;
    try {
        const auto seconds = std::stoul(argv[4]);
        if (seconds < 1 || seconds > 3600)
            throw std::invalid_argument("seconds in [1,3600]");
        check(!siftwing::net::block_termination_signals(), "block before all threads");
        // 首次创建线程可初始化TSan等运行时的常驻后台线程；先完成空线程join再取基线。
        // 仍严格要求所有生产/客户端线程回收到该基线，不过滤任何线程或放宽上限。
        long warmup_tid = 0;
        std::thread runtime_warmup([&] {
            warmup_tid = ::syscall(SYS_gettid);
        });
        runtime_warmup.join();
        // Linux的join唤醒与/proc条目最终移除并非同一观察时点，先等预热线程条目消失。
        await([&] {
            return !std::filesystem::exists("/proc/self/task/" + std::to_string(warmup_tid));
        });
        const auto fds = fd_count(), tasks = threads();
        const std::string mode = argv[2];
        Json summary;
        if (mode == "faults")
            summary = faults(argv[1]);
        else if (mode == "soak") {
            const auto capacity_scale = std::stoul(argv[3]);
            if (capacity_scale != 1 && capacity_scale != 2)
                throw std::invalid_argument("capacity_scale in {1,2}");
            summary = soak(argv[1], capacity_scale, static_cast<unsigned>(seconds));
        } else
            throw std::invalid_argument("unknown suite");
        check(fd_count() == fds, "all input/socket/control fds reclaimed");
        await([&] {
            return threads() == tasks;
        });
        summary["suite"] = mode;
        summary["checks"] = checks;
        summary["fds_after"] = fds;
        summary["threads_after"] = tasks;
        std::cout << summary.dump() << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
