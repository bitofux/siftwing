/*
 * PROJECT : SIFTWING
 * FILE    : versioned_lru_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 逐分配失败证明插入/替换/get的旧内容、顺序和统计保持
 */
#include "siftwing/cache/versioned_lru.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
namespace {
thread_local bool observe = false;
thread_local unsigned int allocations = 0, fail_at = 0;
unsigned int checks = 0;
void check(bool ok, const char *why) {
    ++checks; if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}
}
// 普通全局new与malloc/free成对替换，不覆盖对齐分配/直接malloc或真实OOM。
void *operator new(std::size_t n) {
    if (observe && ++allocations == fail_at) throw std::bad_alloc();
    if (auto *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
int main() {
    using namespace siftwing::cache;
    const CacheKey a{1, Operation::search, std::string(128, 'a'), std::string(64, 'p')};
    const CacheKey b{2, Operation::suggest, std::string(128, 'b'), std::string(64, 'q')};
    const CacheKey c{3, Operation::search, std::string(128, 'c'), std::string(64, 'r')};
    const std::string body(256, 'v'), changed(300, 'w');
    // 对每种操作先观测实际分配数，再逐位置失败；不靠猜测容器的分配布局。
    for (unsigned int mode = 0; mode < 3; ++mode) {
        unsigned int count = 0;
        for (unsigned int position = 0; position <= count; ++position) {
            VersionedLru cache({2, 4096, 1024});
            check(cache.put(a, body) && cache.put(b, body), "initial values");
            const auto old = cache.stats();
            allocations = 0; fail_at = position; observe = true; bool caught = false;
            try {
                if (mode == 0) (void)cache.put(c, changed);
                else if (mode == 1) (void)cache.put(a, changed);
                else (void)cache.get(a);
            } catch (const std::bad_alloc &) { caught = true; }
            observe = false; fail_at = 0;
            if (position == 0) { count = allocations; check(count > 0 && !caught, "allocation observed"); continue; }
            const auto now = cache.stats();
            check(caught && allocations == position && old.entries == now.entries && old.bytes == now.bytes
                && old.hits == now.hits && old.misses == now.misses && old.evictions == now.evictions,
                "failed operation preserves report and size");
            check(cache.put(c, body) && !cache.get(a) && cache.get(b) == body,
                "failed operation preserves LRU order and untouched contents");
        }
    }
    std::cout << "LRU allocation failure checks=" << checks << '\n';
}
