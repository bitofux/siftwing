/*
 * PROJECT : SIFTWING
 * FILE    : server_metrics.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 固定桶延迟聚合与观测失败隔离
 */
#include "siftwing/metrics/server_metrics.hpp"
#include <algorithm>

namespace siftwing::metrics {
void Latency::observe(std::uint64_t elapsed_us, bool success) noexcept {
    count = add(count, 1);
    if (!success)
        failures = add(failures, 1);
    sum_us = add(sum_us, elapsed_us);
    max_us = std::max(max_us, elapsed_us);
    const auto bucket = static_cast<std::size_t>(
        std::lower_bound(latency_bounds_us.begin(), latency_bounds_us.end(), elapsed_us)
        - latency_bounds_us.begin());
    buckets[bucket] = add(buckets[bucket], 1);
}
void QueryCounters::record(QueryKind kind, std::uint64_t elapsed_us, bool success) noexcept {
    try {
        const std::lock_guard<std::mutex> lock(mutex_);
        (kind == QueryKind::search ? totals_.search : totals_.suggest).observe(elapsed_us, success);
    } catch (...) {
        failed_.store(true, std::memory_order_relaxed);
    }
}
QueryTotals QueryCounters::snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto copy = totals_;
    copy.collection_failed = failed_.load(std::memory_order_relaxed);
    return copy;
}
} // namespace siftwing::metrics
