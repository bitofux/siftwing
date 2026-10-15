/*
 * PROJECT : SIFTWING
 * FILE    : versioned_lru_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 独立vector模型验证精确键、最近使用和双重预算，Release仍有效
 */
#include "siftwing/cache/versioned_lru.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
using namespace siftwing::cache;
unsigned int checks = 0;
void check(bool ok, const char *why) {
    ++checks; if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}
bool same(const CacheKey &a, const CacheKey &b) {
    return a.index_version == b.index_version && a.operation == b.operation
        && a.query == b.query && a.parameters == b.parameters;
}
struct Model {
    struct Item { CacheKey key; std::string body; };
    std::vector<Item> items; // 最旧在前；与生产list方向、数据结构及定位算法不同。
    LruStats report;
    void measure() {
        report.entries = items.size(); report.bytes = 0;
        for (const auto &v : items) report.bytes += 2 * (v.key.query.size() + v.key.parameters.size()) + v.body.size();
    }
    auto find(const CacheKey &key) {
        return std::find_if(items.begin(), items.end(), [&](const Item &v) { return same(v.key, key); });
    }
    std::optional<std::string> get(const CacheKey &key) {
        auto p = find(key);
        if (p == items.end()) { ++report.misses; return std::nullopt; }
        const auto copy = *p; items.erase(p); items.push_back(copy); ++report.hits; return copy.body;
    }
    bool put(const CacheKey &key, const std::string &body) {
        if (body.size() > 25 || 2 * (key.query.size() + key.parameters.size()) + body.size() > 90) return false;
        const auto p = find(key); if (p != items.end()) items.erase(p);
        items.push_back({key, body}); measure();
        while (items.size() > 4 || report.bytes > 90) {
            items.erase(items.begin()); ++report.evictions; measure();
        }
        return true;
    }
};
}
int main() {
    for (const LruConfig bad : {LruConfig{4097, 1024, 16}, {1, 0, 1}, {1, 67108865, 1},
             {1, 1024, 0}, {1, 1024, 1025}, {1, 2097152, 1048577}}) {
        bool rejected = false;
        try { VersionedLru cache(bad); } catch (const std::invalid_argument &) { rejected = true; }
        check(rejected, "invalid config rejected");
    }
    CacheKey a{0, Operation::search, "ab", "c"}, b{0, Operation::search, "a", "bc"};
    VersionedLru off;
    check(!off.put(a, "x") && !off.get(a) && off.stats().misses == 1, "disabled");
    VersionedLru cache({2, 128, 40});
    check(cache.put(a, "first") && cache.put(b, "second"), "delimiter collision isolated");
    auto owned = cache.get(a);
    auto v = a; v.index_version = std::numeric_limits<std::uint64_t>::max();
    check(cache.put(v, "version") && !cache.get(b) && cache.get(a) == "first", "get touches MRU then LRU eviction");
    v.operation = Operation::suggest;
    check(cache.put(v, "suggest") && !cache.get(CacheKey{v.index_version, Operation::search, "ab", "c"}),
        "message type isolated");
    check(cache.put(a, "updated") && cache.stats().entries == 2 && cache.get(a) == "updated", "replace not duplicate");
    check(!cache.put(a, std::string(41, 'x')) && cache.get(a) == "updated", "oversized replacement preserves");
    check(owned == "first", "returned Body independent after replace and eviction");
    VersionedLru exact({3, 10, 10});
    CacheKey empty{};
    check(exact.put(empty, std::string(10, 'a')) && exact.stats().bytes == 10, "exact value and charge limit");
    check(exact.put(empty, "") && exact.get(empty) == "" && exact.stats().bytes == 0, "empty Body is a real hit");
    CacheKey nul{0, Operation::search, std::string("a\0b", 3), ""};
    check(exact.put(nul, "1234") && exact.stats().bytes == 10, "NUL bytes charged by size");
    check(!exact.put(CacheKey{0, Operation::search, "123456", ""}, "") && exact.get(nul) == "1234", "key bytes bound");
    check(exact.put(CacheKey{1, Operation::search, "x", ""}, "a") && !exact.get(nul)
        && exact.stats().evictions >= 1, "bytes evict before entry capacity");
    VersionedLru random({4, 90, 25}); Model model; std::uint32_t seed = 0x230U;
    for (unsigned int n = 0; n < 12000; ++n) {
        seed = seed * 1664525U + 1013904223U;
        const CacheKey key{(seed >> 3U) % 3U, seed & 1U ? Operation::search : Operation::suggest,
            std::string((seed >> 8U) % 8U, static_cast<char>('a' + (seed >> 18U) % 3U)),
            std::to_string((seed >> 22U) % 3U)};
        if (seed % 3U == 0) check(random.get(key) == model.get(key), "model get and recency");
        else {
            const std::string body((seed >> 10U) % 30U, static_cast<char>('a' + seed % 26U));
            check(random.put(key, body) == model.put(key, body), "model admission");
        }
        const auto s = random.stats(); const auto m = model.report;
        check(s.hits == m.hits && s.misses == m.misses && s.evictions == m.evictions
            && s.entries == m.entries && s.bytes == m.bytes, "independent model stats and budgets");
    }
    std::cout << "LRU checks=" << checks << '\n';
}
