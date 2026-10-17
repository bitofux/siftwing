/*
 * PROJECT : SIFTWING
 * FILE    : versioned_lru_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 多线程混合版本/业务域与淘汰，返回副本不得串值
 */
#include "siftwing/cache/versioned_lru.hpp"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
int main() {
    using namespace siftwing::cache;
    VersionedLru cache({16, 4096, 128});
    std::atomic<bool> start{false}, failed{false};
    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < 8; ++t)
        workers.emplace_back([&, t] {
            while (!start.load())
                std::this_thread::yield();
            for (unsigned int i = 0; i < 3000; ++i) {
                const CacheKey key{t,
                    i % 2 ? Operation::search : Operation::suggest,
                    std::to_string(i % 7),
                    "parameters"};
                const auto value =
                    std::to_string(t) + ":" + std::to_string(i % 2) + ":" + key.query;
                if (!cache.put(key, value))
                    failed = true;
                // 另线程可以在put/get之间淘汰，但只要命中必须是同一个精确键的确定值。
                if (auto hit = cache.get(key); hit && *hit != value)
                    failed = true;
                const auto stats = cache.stats();
                if (stats.entries > 16 || stats.bytes > 4096)
                    failed = true;
            }
        });
    start = true;
    for (auto &t : workers)
        t.join();
    const auto s = cache.stats();
    if (failed || s.hits + s.misses != 24000 || !s.evictions)
        return 1;
    std::cout << "LRU concurrent operations=48000 get=" << s.hits + s.misses
              << " evictions=" << s.evictions << '\n';
}
