/*
 * PROJECT : SIFTWING
 * FILE    : versioned_lru.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 分配先于提交，精确键定位与真实LRU淘汰
 * IMPLEMENTATION : list拥有键/Body，map复制键并定位稳定list迭代器；同一mutex保护两者。
 */
#include "siftwing/cache/versioned_lru.hpp"
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>

namespace siftwing::cache {
bool CacheKey::operator<(const CacheKey &other) const noexcept {
    return std::tie(index_version, operation, query, parameters)
           < std::tie(other.index_version, other.operation, other.query, other.parameters);
}
namespace {
void increment(std::uint64_t &value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
LruConfig validate(LruConfig c) {
    if (c.capacity > 4096 || c.bytes == 0 || c.bytes > 64U * 1024U * 1024U || c.max_value_bytes == 0
        || c.max_value_bytes > 1024U * 1024U || c.max_value_bytes > c.bytes)
        throw std::invalid_argument("invalid LRU config");
    return c;
}
}
struct VersionedLru::Impl {
    struct Entry {
        CacheKey key;
        std::string body;
        std::size_t charge;
    };
    const LruConfig config;
    mutable std::mutex mutex;
    std::list<Entry> recent; // front为MRU；splice不分配、不使迭代器失效。
    std::map<CacheKey, std::list<Entry>::iterator> lookup;
    LruStats report;
    explicit Impl(LruConfig c) : config(c) {}
    void evict() noexcept {
        // 临时超额最多一个已完整构造条目；无分配提交后只执行无抛操作。
        while (recent.size() > config.capacity || report.bytes > config.bytes) {
            const auto last = std::prev(recent.end());
            report.bytes -= last->charge;
            lookup.erase(last->key);
            recent.erase(last);
            increment(report.evictions);
        }
        report.entries = recent.size();
    }
};
VersionedLru::VersionedLru(LruConfig config) : state_(std::make_unique<Impl>(validate(config))) {}
VersionedLru::~VersionedLru() noexcept = default;
std::optional<std::string> VersionedLru::get(const CacheKey &key) {
    auto &s = *state_;
    const std::lock_guard<std::mutex> guard(s.mutex);
    const auto found = s.lookup.find(key);
    if (found == s.lookup.end()) {
        increment(s.report.misses);
        return std::nullopt;
    }
    std::optional<std::string> result{found->second->body}; // 复制成功才提交hit/最近使用。
    s.recent.splice(s.recent.begin(), s.recent, found->second);
    increment(s.report.hits);
    return result;
}
bool VersionedLru::put(const CacheKey &key, std::string_view value) {
    auto &s = *state_;
    const auto &c = s.config;
    // 逐次减法准入，任意size_t长度也不发生乘加回绕；两处键复制均计入charge。
    if (c.capacity == 0 || value.size() > c.max_value_bytes || value.size() > c.bytes)
        return false;
    std::size_t left = c.bytes - value.size();
    if (key.query.size() > left / 2)
        return false;
    left -= 2 * key.query.size();
    if (key.parameters.size() > left / 2)
        return false;
    left -= 2 * key.parameters.size();
    const auto charge = c.bytes - left;
    const std::lock_guard<std::mutex> guard(s.mutex);
    const auto found = s.lookup.find(key);
    if (found != s.lookup.end()) {
        std::string replacement(value); // 保留原值/顺序，直到唯一潜在分配完成。
        auto &entry = *found->second;
        s.report.bytes = s.report.bytes - entry.charge + charge;
        entry.body.swap(replacement);
        entry.charge = charge;
        s.recent.splice(s.recent.begin(), s.recent, found->second);
    } else {
        s.recent.push_front({key, std::string(value), charge});
        try {
            s.lookup.emplace(s.recent.front().key, s.recent.begin());
        } catch (...) {
            s.recent.pop_front();
            throw;
        }
        s.report.bytes += charge; // 两个合法charge之和<=128MiB，不溢出size_t。
    }
    s.evict();
    return true;
}
LruStats VersionedLru::stats() const {
    const auto &s = *state_;
    const std::lock_guard<std::mutex> guard(s.mutex);
    return s.report;
}
} // namespace siftwing::cache
