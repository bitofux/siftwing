/*
 * PROJECT : SIFTWING
 * FILE    : snapshot_publication_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 独立成品验证原子发布、旧读者寿命、回滚缓存和真实在线切换
 */
#include "siftwing/net/online_server.hpp"
#include "siftwing/net/signalfd_source.hpp"
#include "siftwing/logging/logger.hpp"
#include "../support/online_server_fixture.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <limits>
#include <new>
#include <csignal>
#include <pthread.h>

using namespace siftwing;
using namespace siftwing::test;
namespace {
using Json = nlohmann::json;
using Published = snapshot::SnapshotPublishResult;
std::atomic<bool> pin_arm{false}, pin_entered{false}, pin_release{true};
std::atomic<bool> load_arm{false}, load_entered{false}, load_release{true}, load_join{false};
std::atomic<unsigned> signal_drains{0};
std::atomic<bool> query_mask_valid{true}, loader_mask_valid{true};
bool termination_blocked() {
    sigset_t set;
    return ::pthread_sigmask(SIG_BLOCK, nullptr, &set) == 0 && ::sigismember(&set, SIGINT) == 1
           && ::sigismember(&set, SIGTERM) == 1;
}
bool signal_suite = false; // main在线程创建前固定，所有join之后才结束。
thread_local bool in_query = false;
thread_local int fail_new = -1;
unsigned checks = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok)
        throw std::runtime_error(label);
}
// 先解除测试门，再让生产控制者/测试线程完成join；异常不遗留无人释放的门。
struct Release {
    ~Release() {
        pin_release = true;
        load_release = true;
    }
};
struct Join {
    std::thread &thread;
    ~Join() {
        pin_release = true;
        load_release = true;
        if (thread.joinable())
            thread.join();
    }
};
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
std::unique_ptr<query::SearchService> service(const std::string &parent) {
    query::SearchServiceConfig config;
    config.parent = parent;
    config.version = 100;
    config.cache.capacity = 32;
    auto result = query::SearchService::create(config);
    if (!result)
        throw std::runtime_error(result.error().context);
    return std::move(result).value();
}
concurrency::SearchTask task(std::uint64_t id, bool suggest = false) {
    net::Frame f{suggest ? net::MessageType::suggest_request : net::MessageType::search_request,
        0,
        id,
        suggest ? R"({"key":"shared"})" : R"({"query":"shared"})"};
    auto result = concurrency::SearchTask::create(f);
    return std::get<concurrency::SearchTask>(std::move(result));
}
Json execute(
    const query::SearchService &s, bool suggest = false, query::CacheObservation *obs = nullptr) {
    auto result = s.execute(task(1, suggest), obs);
    check(std::holds_alternative<std::string>(result), "query success");
    return Json::parse(std::get<std::string>(result));
}
void result(const Json &j, const char *label, std::uint64_t n, bool suggest = false) {
    check(j["snapshot_version"] == std::to_string(n) && j["build_version"] == label,
        "one snapshot for response metadata");
    check(!j["results"].empty(), "expected result exists");
    check(suggest ? j["results"][0]["term"] == std::string("shared-") + label
                  : j["results"][0]["title"] == label,
        "one snapshot for search/recommendation content");
}
snapshot::SnapshotLoadCompletion loaded(
    query::SearchService &s, const std::string &parent, std::uint64_t n) {
    check(s.try_load_candidate({parent, version(n), limits()})
              == snapshot::SnapshotLoadSubmit::accepted,
        "background load accepted");
    std::optional<snapshot::SnapshotLoadCompletion> done;
    await([&] {
        done = s.take_loaded_candidate();
        return bool(done);
    });
    check(done->requested_version == n && done->report && !done->exception,
        "load completion identity");
    return std::move(*done);
}
void managers(const std::string &parent) {
    auto opened = snapshot::SnapshotManager::open(parent, version(100), limits());
    check(bool(opened), "manager initial");
    auto m = std::move(opened).value();
    auto old = m->current();
    std::weak_ptr<const snapshot::SnapshotReader> weak = old;
    auto next = m->load_candidate(parent, version(101), limits());
    check(m->current() == old, "load does not publish");
    fail_new = 0;
    auto published = m->publish_candidate(next->outcome.value());
    const bool no_allocation = fail_new == 0;
    fail_new = -1;
    check(published == Published::published && no_allocation, "publication allocates nothing");
    check(m->publish_candidate(m->current()) == Published::unchanged, "same object unchanged");
    check(m->publish_candidate({}) == Published::invalid_candidate, "null candidate rejected");
    auto movable = snapshot::SnapshotReader::open(parent, version(100), limits());
    auto empty = std::make_shared<snapshot::SnapshotReader>(std::move(movable).value());
    auto moved = std::move(*empty);
    check(
        m->publish_candidate(empty) == Published::invalid_candidate, "moved-from reader rejected");
    check(m->current() == next->outcome.value() && old->manifest()->build_version == "old",
        "rejection keeps new, held old remains complete");
    // 多读者只观察完整旧/新组合；与发布同时运行，引用计数不是具体数值合同。
    std::atomic<bool> stop{false}, good{true};
    std::atomic<unsigned> reads{0};
    std::vector<std::thread> threads;
    const auto finish = [&] {
        stop = true;
        for (auto &t : threads)
            t.join();
    };
    try {
        for (unsigned i = 0; i < 8; ++i)
            threads.emplace_back([&] {
                unsigned count = 0;
                do {
                    const auto h = m->current();
                    const auto &meta = *h->manifest();
                    const auto expected = meta.snapshot.value() == 100 ? 11U : 22U;
                    if (!h->document(base::DocumentId::from_integer(expected).value()))
                        good = false;
                    ++count;
                } while (!stop.load() || count < 1000);
                reads += count;
            });
        for (unsigned i = 0; i < 1000; ++i)
            (void)m->publish_candidate(i % 2 ? old : next->outcome.value());
    } catch (...) {
        finish();
        throw;
    }
    finish();
    check(good && reads >= 8000, "concurrent readers never mix versions");
    (void)m->publish_candidate(next->outcome.value());
    old.reset();
    check(weak.expired(), "unheld old snapshot released");
    const auto survivor = m->current();
    m.reset();
    check(survivor->manifest()->snapshot.value() == 101, "published handle survives manager");
    std::cout << "publication reads=" << reads << '\n';
}
void queries(const std::string &parent, const std::string &alias, const std::string &bad) {
    auto s = service(parent);
    auto old = s->current_snapshot();
    // 冷缓存的旧查询在取得拥有句柄后暂停；新版本先完成，再恢复旧计算的晚到缓存写。
    pin_entered = false;
    pin_release = false;
    pin_arm = true;
    query::SearchServiceResult old_result = net::ErrorStatus::internal_error;
    std::optional<std::uint64_t> used;
    std::exception_ptr error;
    std::thread thread([&] {
        try {
            old_result = s->execute(task(7), nullptr, &used);
        } catch (...) {
            error = std::current_exception();
        }
    });
    Join join{thread};
    await([] {
        return pin_entered.load();
    });
    auto completion = loaded(*s, parent, 101);
    check(s->current_snapshot() == old, "completed load still old");
    check(s->publish_candidate(completion.report->outcome.value()) == Published::published,
        "service publish");
    result(execute(*s), "new", 101);
    result(execute(*s, true), "new", 101, true);
    auto same_version = loaded(*s, alias, 100);
    check(s->publish_candidate(same_version.report->outcome.value()) == Published::published,
        "same version different content accepted");
    query::CacheObservation obs;
    result(execute(*s, false, &obs), "alias", 100);
    check(obs == query::CacheObservation::miss, "same numeric version content isolated");
    pin_release = true;
    thread.join();
    if (error)
        std::rethrow_exception(error);
    check(used == 100 && std::holds_alternative<std::string>(old_result),
        "actual used version output");
    result(Json::parse(std::get<std::string>(old_result)), "old", 100);
    result(execute(*s, false, &obs), "alias", 100);
    check(obs == query::CacheObservation::hit, "late old put cannot overwrite alias cache");
    (void)s->publish_candidate(old);
    result(execute(*s, false, &obs), "old", 100);
    check(obs == query::CacheObservation::hit, "rollback can reuse exact old content cache");
    const auto rejected = loaded(*s, bad, 101);
    check(!rejected.report->outcome && s->current_snapshot() == old,
        "failed load preserves online view");
    for (auto n : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
        const auto done = loaded(*s, parent, n);
        (void)s->publish_candidate(done.report->outcome.value());
        result(execute(*s), n == 0 ? "zero" : "maximum", n);
    }
    const auto empty = loaded(*s, parent, 2);
    (void)s->publish_candidate(empty.report->outcome.value());
    check(execute(*s)["results"].empty() && execute(*s, true)["results"].empty(),
        "empty published snapshot");
    s->stop_loading();
    s->stop_loading();
    check(s->try_load_candidate({parent, version(101), limits()})
              == snapshot::SnapshotLoadSubmit::stopped,
        "load shutdown permanent");
    // 首次创建Loader失败发生在任务被接受之前；服务仍可查询并重试。
    auto fresh = service(parent);
    snapshot::SnapshotLoadRequest request{parent, version(101), limits()};
    fail_new = 0;
    bool threw = false;
    try {
        (void)fresh->try_load_candidate(std::move(request));
    } catch (const std::bad_alloc &) {
        threw = true;
    }
    fail_new = -1;
    check(threw && fresh->index_version() == 100, "first loader allocation failure keeps service");
    check(
        loaded(*fresh, parent, 101).report->outcome.has_value(), "retry after allocation failure");
}
Json response(int fd, std::uint16_t type, std::uint64_t id) {
    const auto h = receive(fd, 24);
    std::size_t size = 0;
    for (std::size_t i = 12; i < 16; ++i)
        size = size * 256 + static_cast<unsigned char>(h[i]);
    check(size <= net::frame_max_body_bytes, "response budget");
    const auto b = receive(fd, size);
    check(h + b == wire(type, id, b), "independent wire header/type/id/status");
    return Json::parse(b);
}
void online(const std::string &parent, const std::string &log_path) {
    auto logging = logging::Logger::create(logging::Level::debug, log_path);
    check(bool(logging), "logger ready");
    auto logger = std::move(logging).value();
    net::OnlineServerConfig config;
    config.workers = 1;
    config.connection.max_connections = 256;
    config.connection.completion_capacity = 256;
    config.logger = logger.get();
    auto created = net::OnlineServer::create(service(parent), config);
    auto server = std::move(std::get<std::unique_ptr<net::OnlineServer>>(created));
    siftwing::test::OnlineServerDriver drive(*server);
    Release release;
    unsigned connected = 0;
    const auto attach = [&] {
        auto fd = client(server->port());
        ++connected;
        await([&] {
            const auto e = server->run_once(0);
            if (e)
                throw std::system_error(e);
            return server->report().reactor.admitted == connected;
        });
        return fd;
    };
    auto old = server->current_snapshot();
    auto running = attach();
    auto queued = attach();
    auto info = attach();
    // 一个worker先停在取得旧快照后；第二请求确定入队但未执行。
    pin_entered = false;
    pin_release = false;
    pin_arm = true;
    send_all(running.get(), wire(3, 71, R"({"query":"shared"})"));
    await([] {
        return pin_entered.load();
    });
    send_all(queued.get(), wire(4, 72, R"({"key":"shared"})"));
    await([&] {
        return server->metrics().queues.tasks == 1;
    });
    check(server->try_load_snapshot({parent, version(101), limits()})
              == snapshot::SnapshotLoadSubmit::accepted,
        "online asynchronous load accepted");
    check(server->try_load_snapshot({parent, version(101), limits()})
              == snapshot::SnapshotLoadSubmit::busy,
        "online single slot busy");
    std::optional<snapshot::SnapshotLoadCompletion> done;
    await([&] {
        done = server->take_loaded_snapshot();
        return bool(done);
    });
    check(done->report && done->report->outcome && server->metrics().index.version == 100,
        "online completion not automatically published");
    check(std::get<Published>(server->publish_snapshot(done->report->outcome.value()))
              == Published::published,
        "online explicit publish");
    send_all(info.get(), wire(2, 73));
    const auto metadata = response(info.get(), 0x8002, 73);
    check(metadata["snapshot_version"] == "101" && metadata["build_version"] == "new",
        "live SERVER_INFO reflects published metadata");
    check(server->metrics().index.version == 101 && server->metrics().index.documents == 2,
        "live index metrics reflect same manifest");
    pin_release = true;
    result(response(running.get(), 0x8003, 71), "old", 100);
    result(response(queued.get(), 0x8004, 72), "new", 101, true);
    (void)server->publish_snapshot(old);
    send_all(info.get(), wire(2, 74));
    check(
        response(info.get(), 0x8002, 74)["snapshot_version"] == "100", "network rollback visible");
    check(server->try_load_snapshot({parent, version(101), limits()})
              == snapshot::SnapshotLoadSubmit::accepted,
        "accepted load before shutdown");
    check(!server->shutdown(1000), "shutdown joins loader and query publishers");
    auto stopped_done = server->take_loaded_snapshot();
    check(stopped_done && stopped_done->report && stopped_done->report->outcome,
        "shutdown drains accepted load, completion retained");
    check(server->try_load_snapshot({parent, version(101), limits()})
              == snapshot::SnapshotLoadSubmit::stopped,
        "stopped server rejects load");
    check(std::get<std::error_code>(server->publish_snapshot(old)).value() == ECANCELED,
        "stopped server rejects publication");
    drive.release();
    server.reset();
    check(!logger->failed(), "dynamic version logging valid");
    logger.reset();
    std::ifstream input(log_path);
    std::string line;
    unsigned found = 0;
    while (std::getline(input, line)) {
        const auto j = Json::parse(line);
        if (j["event"] == "query_finished"
            && (j["request_id"] == "71" || j["request_id"] == "72")) {
            check(j["index_version"] == (j["request_id"] == "71" ? "100" : "101"),
                "business log uses executed version, never a separate sample");
            ++found;
        }
    }
    check(found == 2, "both actual execution logs found");
}
// 真实在途/排队查询及正在加载候选同时遇终止信号；门禁只控制时序，仍执行完整业务。
void signaled_online(const std::string &parent,
    int signo,
    bool timeout,
    const std::string &candidate_parent,
    bool valid_candidate) {
    net::OnlineServerConfig config;
    config.workers = 1;
    config.connection.max_connections = 256;
    config.connection.completion_capacity = 256;
    config.reactor.termination_signals = true;
    auto created = net::OnlineServer::create(service(parent), config);
    if (auto *error = std::get_if<std::error_code>(&created))
        throw std::system_error(*error);
    auto server = std::move(std::get<std::unique_ptr<net::OnlineServer>>(created));
    siftwing::test::OnlineServerDriver drive(*server);
    Release release;
    check(server->shutdown_signal() == 0, "no signal observed before run");
    unsigned connected = 0;
    const auto attach = [&] {
        auto fd = client(server->port());
        ++connected;
        await([&] {
            if (const auto error = server->run_once(0))
                throw std::system_error(error);
            return server->report().reactor.admitted == connected;
        });
        return fd;
    };
    auto running = attach(), queued = attach();
    pin_entered = false;
    pin_release = false;
    pin_arm = true;
    send_all(running.get(), wire(3, 91, R"({"query":"shared"})"));
    await([] {
        return pin_entered.load();
    });
    send_all(queued.get(), wire(4, 92, R"({"key":"shared"})"));
    await([&] {
        return server->metrics().queues.tasks == 1;
    });
    load_entered = false;
    load_release = false;
    load_join = false;
    load_arm = true;
    check(server->try_load_snapshot({candidate_parent, version(101), limits()})
              == snapshot::SnapshotLoadSubmit::accepted,
        "real loader accepted before signal");
    await([] {
        return load_entered.load();
    });
    check(::kill(::getpid(), signo) == 0, "send termination to isolated integration process");
    check(!server->run_once(100) && server->shutdown_signal() == signo && !server->report().stopped,
        "signal is successful control request, no premature shutdown");
    check(!server->run_once(0) && server->shutdown_signal() == signo
              && server->report().accepted == 2,
        "repeat control observation does not accept again");
    signal_drains = 0;
    std::exception_ptr observer_error;
    std::thread observer([&] {
        Release always_release;
        try {
            await([] {
                return signal_drains.load() >= 1;
            });
            if (!timeout) {
                pin_release = true;
                const auto search = response(running.get(), 0x8003, 91);
                const auto suggest = response(queued.get(), 0x8004, 92);
                if (search["snapshot_version"] != "100" || search["results"][0]["title"] != "old"
                    || suggest["snapshot_version"] != "100"
                    || suggest["results"][0]["term"] != "shared-old")
                    throw std::runtime_error("signal drain changed real query results");
            }
            // 确认控制者已走到加载器join；加载未释放前shutdown不能完成。
            await([] {
                return load_join.load();
            });
            load_release = true;
            pin_release = true;
        } catch (...) {
            observer_error = std::current_exception();
        }
    });
    Join join{observer};
    const auto error = server->shutdown(timeout ? 0 : 5000);
    observer.join();
    if (observer_error)
        std::rethrow_exception(observer_error);
    check(!error && load_join.load() && query_mask_valid.load() && loader_mask_valid.load(),
        "shutdown drains loader and workers with inherited mask");
    const auto report = server->report();
    check(report.stopped && report.workers.succeeded == 2 && !report.workers.failed
              && !report.first_error,
        "running and queued jobs finish once, no signal failure");
    std::size_t responses = 0, timeouts = 0;
    for (const auto &sub : {report.reactor}) {
        check(sub.state == net::ReactorState::stopped && sub.active == 0, "Reactor owner joined");
        responses += sub.drain_responses;
        timeouts += sub.drain_timeouts;
    }
    check(
        timeout ? timeouts == 2 : responses == 2, "normal drain or exact zero deadline preserved");
    auto done = server->take_loaded_snapshot();
    check(done && done->report && done->report->outcome.has_value() == valid_candidate
              && server->current_snapshot()->manifest()->snapshot == version(100),
        "accepted load retained, never automatically published");
    check(server->shutdown_signal() == signo && !server->shutdown(-1) && !server->stop(),
        "signal remains observable after idempotent close");
}

} // namespace

// GNU ld门禁仍调用真实实现；current在真实原子load之后暂停，execute不伪造业务结果。
extern "C" snapshot::SnapshotHandle real_current(const snapshot::SnapshotManager *) __asm__(
    "__real__ZNK8siftwing8snapshot15SnapshotManager7currentEv");
extern "C" snapshot::SnapshotHandle wrapped_current(const snapshot::SnapshotManager *) __asm__(
    "__wrap__ZNK8siftwing8snapshot15SnapshotManager7currentEv");
extern "C" snapshot::SnapshotHandle wrapped_current(const snapshot::SnapshotManager *m) {
    auto h = real_current(m);
    if (in_query && pin_arm.exchange(false)) {
        pin_entered = true;
        while (!pin_release.load())
            std::this_thread::yield();
    }
    return h;
}
query::SearchServiceResult real_execute(const query::SearchService *,
    const concurrency::SearchTask &,
    query::CacheObservation *,
    std::optional<std::uint64_t>
        *) __asm__("__real__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE");
query::SearchServiceResult wrapped_execute(const query::SearchService *,
    const concurrency::SearchTask &,
    query::CacheObservation *,
    std::optional<std::uint64_t>
        *) __asm__("__wrap__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE");
query::SearchServiceResult wrapped_execute(const query::SearchService *s,
    const concurrency::SearchTask &t,
    query::CacheObservation *o,
    std::optional<std::uint64_t> *v) {
    if (signal_suite && !termination_blocked())
        query_mask_valid = false;
    struct InQuery {
        InQuery() {
            in_query = true;
        }
        ~InQuery() {
            in_query = false;
        }
    } scope;
    return real_execute(s, t, o, v);
}
extern "C" void *real_new(std::size_t) __asm__("__real__Znwm");
extern "C" void *wrapped_new(std::size_t) __asm__("__wrap__Znwm");
extern "C" void *wrapped_new(std::size_t n) {
    if (fail_new == 0) {
        fail_new = -1;
        throw std::bad_alloc();
    }
    if (fail_new > 0)
        --fail_new;
    return real_new(n);
}
std::shared_ptr<const snapshot::SnapshotLoadResult> real_load(snapshot::SnapshotManager *,
    const std::string &,
    base::SnapshotVersion,
    const snapshot::SnapshotReadLimits
        &) __asm__("__real__ZN8siftwing8snapshot15SnapshotManager14load_candidateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS_4base6detail8StrongIdINSB_18SnapshotVersionTagEEERKNS0_18SnapshotReadLimitsE");
std::shared_ptr<const snapshot::SnapshotLoadResult> wrapped_load(snapshot::SnapshotManager *,
    const std::string &,
    base::SnapshotVersion,
    const snapshot::SnapshotReadLimits
        &) __asm__("__wrap__ZN8siftwing8snapshot15SnapshotManager14load_candidateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS_4base6detail8StrongIdINSB_18SnapshotVersionTagEEERKNS0_18SnapshotReadLimitsE");
std::shared_ptr<const snapshot::SnapshotLoadResult> wrapped_load(snapshot::SnapshotManager *m,
    const std::string &parent,
    base::SnapshotVersion v,
    const snapshot::SnapshotReadLimits &l) {
    if (load_arm.exchange(false)) {
        if (!termination_blocked())
            loader_mask_valid = false;
        load_entered = true;
        while (!load_release.load())
            std::this_thread::yield();
    }
    return real_load(m, parent, v, l);
}
void real_loader_join(snapshot::SnapshotLoader *) __asm__(
    "__real__ZN8siftwing8snapshot14SnapshotLoader4joinEv");
void wrapped_loader_join(snapshot::SnapshotLoader *) __asm__(
    "__wrap__ZN8siftwing8snapshot14SnapshotLoader4joinEv");
void wrapped_loader_join(snapshot::SnapshotLoader *loader) {
    load_join = true;
    real_loader_join(loader);
}
void real_signal_drain(concurrency::WorkerPool *) __asm__(
    "__real__ZN8siftwing11concurrency10WorkerPool12request_stopEv");
void wrapped_signal_drain(concurrency::WorkerPool *) __asm__(
    "__wrap__ZN8siftwing11concurrency10WorkerPool12request_stopEv");
void wrapped_signal_drain(concurrency::WorkerPool *pool) {
    real_signal_drain(pool); // 唯一I/O排空的实际任务准入关闭点，不再等待两个Reactor通知。
    ++signal_drains;
}
int main(int argc, char **argv) {
    if (argc != 5 && argc != 6)
        return 2;
    Release release;
    try {
        const auto baseline = fd_count();
        signal_suite = argc == 6;
        if (signal_suite) {
            check(!net::block_termination_signals(), "block before creating any test threads");
            const bool races = std::string(argv[5]) == "races";
            check(races || std::string(argv[5]) == "signals", "known signal suite");
            // 重复四种确定时序：合法/失败候选 × 正常排空/零网络期限；每次建服并回收。
            for (unsigned round = 0; round < (races ? 8U : 1U); ++round) {
                signaled_online(argv[1], SIGTERM, false, argv[1], true);
                signaled_online(argv[1], SIGINT, true, argv[1], true);
                if (races) {
                    signaled_online(argv[1], SIGINT, false, argv[3], false);
                    signaled_online(argv[1], SIGTERM, true, argv[3], false);
                }
                check(fd_count() == baseline, "each signal shutdown cycle reclaims all fds");
            }
        } else {
            managers(argv[1]);
            queries(argv[1], argv[2], argv[3]);
            online(argv[1], argv[4]);
        }
        check(fd_count() == baseline, "all input/network/notification fds reclaimed");
        std::cout << "snapshot publication checks=" << checks << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
