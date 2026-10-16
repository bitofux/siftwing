/*
 * PROJECT : SIFTWING
 * FILE    : server_metrics.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 有界计数、查询延迟聚合和无业务借用的服务指标值
 */
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

namespace siftwing::metrics {
/** @brief 无符号累计饱和相加；不回绕，不提供对引用对象的线程同步。 */
[[nodiscard]] constexpr std::uint64_t add(std::uint64_t value, std::uint64_t delta) noexcept {
    return delta > std::numeric_limits<std::uint64_t>::max() - value
               ? std::numeric_limits<std::uint64_t>::max()
               : value + delta;
}
/** @brief 互斥延迟桶的五个包含端点上界，单位微秒；第六桶为大于最后上界。 */
inline constexpr std::array<std::uint64_t, 5> latency_bounds_us{100, 1000, 10000, 100000, 1000000};
/** @brief 调用者拥有的延迟值；桶非累积，不由本类型自动同步并发写。 */
struct Latency final {
    std::uint64_t count = 0, failures = 0; ///< 已观察execute次数与其中非成功业务结果次数。
    std::uint64_t sum_us = 0, max_us = 0; ///< steady_clock服务耗时之和/最大值，含失败调用。
    std::array<std::uint64_t, 6> buckets{}; ///< 每次只进一个桶；未饱和时总和等于count。
    /** @brief 记录一次非负微秒耗时；累计字段饱和，零耗时合法，失败也计延迟。 */
    void observe(std::uint64_t elapsed_us, bool success) noexcept;
};
/** @brief 两种生产查询，禁止将PING或SERVER_INFO的本地处理混入worker服务延迟。 */
enum class QueryKind {
    search,
    suggest
};
/** @brief 同一收集器锁下的查询统计值；collection_failed表示至少一次记录因同步异常丢失。 */
struct QueryTotals final {
    Latency search, suggest; ///< 独立业务域；不包括任务排队、结果发布及网络发送。
    bool collection_failed = false; ///< 单向故障观察，不表示业务执行失败。
};
/**
 * @brief 服务独占、worker稳定借用的固定大小查询收集器，无分配或业务对象所有权
 * @note record/snapshot可并发，析构前撤调用并join；不与业务/队列/日志锁嵌套。
 */
class QueryCounters final {
public:
    /** @brief 只更新本调用对应组；同步异常吞并并置collection_failed，业务结果保持。 */
    void record(QueryKind kind, std::uint64_t elapsed_us, bool success) noexcept;
    /** @brief 同锁复制两组直方图与计数；锁异常传播，由观察调用方隔离。 */
    [[nodiscard]] QueryTotals snapshot() const;

private:
    mutable std::mutex mutex_; ///< 只围住固定值复制/累计，不围住execute或输出。
    QueryTotals totals_; ///< 受mutex保护；failure的单向原子镜像在复制时附入。
    std::atomic<bool> failed_{false}; ///< 锁失败也能记录；不发布其他业务状态。
};
/** @brief Connection所属线程的累计I/O值；close/失败清空缓冲也不清零这些历史量。 */
struct ConnectionIo final {
    std::uint64_t received_bytes = 0,
                  sent_bytes = 0; ///< 实际正recv/send字节，含帧头/坏输入/部分输出。
    std::uint64_t input_high_bytes = 0,
                  output_high_bytes = 0; ///< 单连接用户缓冲可读字节峰值，非capacity/RSS。
};
/** @brief 单次从同一只读索引清单取得的真实元数据；版本不表示时间，搜索与推荐词项数分开。 */
struct Index final {
    std::uint64_t version = 0, documents = 0, terms = 0, recommendation_terms = 0;
};
/**
 * @brief 控制线程汇总的拥有值；各组件采样时点不同，不是全局原子快照
 * @note 累计值饱和，当前深度/活跃数是观测值；无连接、任务、快照或字符串借用。
 *     运行时不能据跨组件等式证明丢失或静止；关闭屏障完成后的值才稳定。
 */
struct Snapshot final {
    struct Connections {
        std::uint64_t accepted = 0, handed_off = 0, rejected = 0, admitted = 0;
        std::uint64_t closed = 0, active = 0, discarded = 0;
    } connections; ///< rejected合并Main拒绝和Sub注册前拒绝，discarded为未注册移交残留。
    struct Network {
        ConnectionIo io;
        std::uint64_t input_bytes = 0, output_bytes = 0; ///< 已发布Sub快照中的当前缓冲字节总量。
        std::uint64_t protocol_errors = 0; ///< 坏头/截帧/Body准入拒绝；不含业务schema错误。
    } network;
    struct Requests {
        std::array<std::uint64_t, 4>
            by_type{}; ///< PING/INFO/SEARCH/SUGGEST的完整受信交付次数，含坏Body。
        std::array<std::uint64_t, 7> responses{}; ///< 实际入输出队列的status 0..6次数，非对端送达。
        std::uint64_t drained = 0; ///< 输出排空且finish成功，不证明对端收到。
    } requests;
    struct Queues {
        std::uint64_t tasks = 0, task_capacity = 0, handoff = 0, completions = 0;
    } queues; ///< 各队列自己锁下的即时深度；不包括已取出的任务/结果。
    struct Workers {
        std::uint64_t count = 0, busy = 0, succeeded = 0, failed = 0;
    } workers;           ///< busy覆盖execute调用，不包括任务析构或线程join。
    QueryTotals queries; ///< 搜索/推荐服务延迟；不通过日志条数或全局缓存差推测。
    struct Cache {
        std::uint64_t hits = 0, misses = 0, evictions = 0, entries = 0, bytes = 0;
    } cache; ///< LRU同锁统计；bytes是charge，不是进程RSS。
    Index index;
    struct Shutdown {
        bool started = false, finished = false;
        std::uint64_t elapsed_us = 0, drain_timeouts = 0, drain_responses = 0;
        std::uint64_t stale_completions = 0, discarded_completions = 0;
    } shutdown; ///< elapsed包括完整worker/Sub屏障，不是网络deadline或总退出上限。
};
} // namespace siftwing::metrics
