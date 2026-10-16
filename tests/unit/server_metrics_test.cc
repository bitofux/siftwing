/*
 * PROJECT : SIFTWING
 * FILE    : server_metrics_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 独立核延迟桶、并发采样、真实socket字节及指标输出的失败边界
 */
#include "siftwing/metrics/server_metrics.hpp"
#include "siftwing/logging/logger.hpp"
#include "siftwing/net/connection.hpp"
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local bool fail_allocation = false, fail_lock = false;
void check(bool value, const char *label) {
    if (!value)
        throw std::runtime_error(label);
}
using namespace siftwing;
using Json = nlohmann::json;
void latency() {
    metrics::Latency value;
    // 显式端点参考与预期互斥桶，不复用生产lower_bound作参考。
    const std::array<std::uint64_t, 11> times{
        0, 100, 101, 1000, 1001, 10000, 10001, 100000, 100001, 1000000, 1000001};
    for (std::size_t i = 0; i < times.size(); ++i)
        value.observe(times[i], i % 3 != 0);
    check(
        value.count == 11 && value.failures == 4 && value.max_us == 1000001, "延迟次数/失败/峰值");
    check(value.buckets == std::array<std::uint64_t, 6>{2, 2, 2, 2, 2, 1}, "含端点的互斥桶");
    check(value.sum_us == std::accumulate(times.begin(), times.end(), std::uint64_t{0}),
        "独立耗时和");
    value.count = UINT64_MAX - 1;
    value.failures = UINT64_MAX;
    value.buckets.back() = UINT64_MAX - 1;
    value.observe(UINT64_MAX, false);
    value.observe(UINT64_MAX, false);
    check(value.count == UINT64_MAX && value.failures == UINT64_MAX && value.sum_us == UINT64_MAX
              && value.max_us == UINT64_MAX && value.buckets.back() == UINT64_MAX,
        "累计不回绕");
    check(metrics::add(UINT64_MAX, 1) == UINT64_MAX && metrics::add(1, UINT64_MAX) == UINT64_MAX,
        "饱和加法两侧边界");
}
void concurrency() {
    metrics::QueryCounters counters;
    std::atomic<unsigned> done{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i)
        workers.emplace_back([&, i] {
            for (unsigned n = 0; n < 1000; ++n)
                counters.record(i % 2 ? metrics::QueryKind::suggest : metrics::QueryKind::search,
                    i % 2 ? 101 : 100,
                    n % 4 != 0);
            ++done;
        });
    bool coherent = true;
    while (done.load() != 8) {
        const auto current = counters.snapshot();
        for (const auto &group : {current.search, current.suggest})
            coherent = coherent
                       && group.count
                              == std::accumulate(
                                  group.buckets.begin(), group.buckets.end(), std::uint64_t{0})
                       && group.failures <= group.count;
        std::this_thread::yield();
    }
    for (auto &worker : workers)
        worker.join();
    const auto final = counters.snapshot();
    check(coherent && !final.collection_failed, "并发采样的每组直方图一致");
    check(final.search.count == 4000 && final.suggest.count == 4000 && final.search.failures == 1000
              && final.suggest.failures == 1000 && final.search.sum_us == 400000
              && final.suggest.sum_us == 404000 && final.search.buckets[0] == 4000
              && final.suggest.buckets[1] == 4000,
        "8线程的独立计数参考");
    fail_lock = true;
    counters.record(metrics::QueryKind::search, 10, true);
    const auto rejected = counters.snapshot();
    check(rejected.collection_failed && rejected.search.count == 4000,
        "同步观察失败不传播并标明丢失");
}
std::string ping() {
    // 独立手写24字节帧头和2字节Body，不用生产编码器推导真实I/O预期。
    std::string packet(26, '\0');
    packet.replace(0, 4, "SFTW");
    packet[4] = 1;
    packet[5] = 24;
    packet[7] = 1;
    packet[15] = 2;
    packet[23] = 7;
    packet[24] = '{';
    packet[25] = '}';
    return packet;
}
void connection(bool allocation_failure) {
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
        "socketpair");
    net::UniqueFd source(pair[0]), peer(pair[1]);
    net::Connection session(std::move(source), 1024);
    const auto bytes = ping();
    check(::send(peer.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL) == 26, "写独立完整帧");
    if (allocation_failure) {
        bool caught = false;
        fail_allocation = true;
        try {
            (void)session.read_request(64, 8);
        } catch (const std::bad_alloc &) {
            caught = true;
        }
        fail_allocation = false;
        check(caught && session.get() == -1 && session.io_stats().received_bytes == 26,
            "recv后分配失败仍保留真实字节且关闭连接");
        return;
    }
    check(session.read_request(64, 8).stop == net::ReadStop::request, "准入PING");
    check(
        session.queue_response({net::MessageType::pong, 0, 7, "{}"}).stop == net::QueueStop::queued,
        "PONG入队");
    for (unsigned i = 0; i < 26; ++i)
        check(session.flush_output(1, 1).bytes == 1, "26次真实部分发送");
    check(session.flush_output(64, 8).bytes == 0, "空输出不重计");
    std::array<char, 26> received{};
    check(::recv(peer.get(), received.data(), received.size(), 0) == 26, "对端收26字节");
    const auto observed = session.io_stats();
    check(observed.received_bytes == 26 && observed.sent_bytes == 26
              && observed.input_high_bytes == 26 && observed.output_high_bytes == 26,
        "精确I/O和缓冲峰值");
    check(!session.close() && session.io_stats().sent_bytes == 26
              && session.io_stats().received_bytes == 26,
        "关闭清空缓冲不抹去已发生I/O");
}
void logging_contract(const std::filesystem::path &root) {
    metrics::Snapshot snapshot;
    snapshot.index = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    snapshot.connections = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    snapshot.network = {
        {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    snapshot.requests.by_type.fill(UINT64_MAX);
    snapshot.requests.responses.fill(UINT64_MAX);
    snapshot.requests.drained = UINT64_MAX;
    snapshot.queues = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    snapshot.workers = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    snapshot.queries.search = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, {}};
    snapshot.queries.search.buckets.fill(UINT64_MAX);
    snapshot.queries.suggest = snapshot.queries.search;
    snapshot.cache = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    snapshot.shutdown = {true, true, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    auto created = logging::Logger::create(logging::Level::info, (root / "metrics").string());
    check(bool(created), "指标日志独占目标");
    auto logger = std::move(created).value();
    logger->write_metrics(snapshot, UINT64_MAX);
    check(!logger->failed(), "最大数值快照仍符合原行上限");
    std::ifstream input(root / "metrics");
    std::string line;
    std::set<std::string> families;
    while (std::getline(input, line)) {
        const auto row = Json::parse(line);
        check(line.size() + 1 <= 2048 && row.at("event") == "metrics_snapshot"
                  && row.at("sample_id") == "18446744073709551615"
                  && row.at("index_version") == "18446744073709551615",
            "行界限和完整uint64");
        families.insert(row.at("metric_family").get<std::string>());
        check(row.at("request_id").is_null() && row.at("connection_id").is_null(),
            "无高基数请求标签");
        if (row.at("metric_family") == "requests")
            check(row.at("values").at("responses").size() == 7
                      && row.at("values").at("by_type").size() == 4,
                "固定请求/错误域");
    }
    check(families
              == std::set<std::string>{"connections",
                  "network",
                  "requests",
                  "queues",
                  "workers",
                  "search",
                  "suggest",
                  "cache",
                  "index",
                  "shutdown"},
        "固定10族");
    fail_allocation = true;
    logger->write_metrics(snapshot, 2);
    fail_allocation = false;
    check(logger->failed(), "序列化分配失败只停用日志");
    logger->write_metrics(snapshot, 3);
    auto off = logging::Logger::create(logging::Level::off);
    check(bool(off), "off实例");
    fail_allocation = true;
    off.value()->write_metrics(snapshot, 0);
    check(fail_allocation && !off.value()->failed(), "off不序列化或分配");
    fail_allocation = false;
}
} // namespace

extern "C" void *__real__Znwm(std::size_t);
extern "C" void *__wrap__Znwm(std::size_t size) {
    if (fail_allocation) {
        fail_allocation = false;
        throw std::bad_alloc();
    }
    return __real__Znwm(size);
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (fail_lock) {
        fail_lock = false;
        return EINVAL;
    }
    return __real_pthread_mutex_lock(mutex);
}
int main() {
    char pattern[] = "/tmp/siftwing-metrics-XXXXXX";
    const char *directory = ::mkdtemp(pattern);
    if (!directory)
        return 1;
    const std::filesystem::path root(directory);
    try {
        latency();
        concurrency();
        connection(false);
        connection(true);
        logging_contract(root);
        std::filesystem::remove_all(root);
        std::cout << "metrics: boundaries/concurrency/socket/failures/schema passed\n";
        return 0;
    } catch (const std::exception &error) {
        fail_allocation = fail_lock = false;
        std::cerr << "指标测试失败：" << error.what() << "；夹具保留于" << root << '\n';
        return 1;
    }
}
