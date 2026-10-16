/*
 * PROJECT : SIFTWING
 * FILE    : snapshot_manager_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 用独立八文件输入验证候选隔离、拥有寿命、分配异常和并发读取
 */
#include "siftwing/snapshot/manager.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace siftwing;
namespace {
thread_local int fail_after_open = -1;
thread_local int remaining = -1;
unsigned checks = 0;
void require(bool ok, const char *label) {
    ++checks;
    if (!ok)
        throw std::runtime_error(label);
}
base::SnapshotVersion version(std::uint64_t value) {
    return base::SnapshotVersion::from_integer(value).value();
}
snapshot::SnapshotReadLimits limits() {
    constexpr std::uint64_t bytes = 1ULL << 26U;
    const auto huge = std::numeric_limits<std::uint64_t>::max();
    return {{{bytes, bytes}, 1000000, bytes, bytes},
        {bytes, bytes, bytes},
        {{1000000, bytes, bytes, 1000000, bytes}, 1000000, huge, huge, 1000000},
        {{1000000, bytes, bytes, 1000000, bytes}, huge, huge, 1000000, 1000000}};
}
bool valid(const snapshot::SnapshotHandle &handle, std::uint64_t expected) {
    if (!handle || !handle->manifest() || handle->manifest()->snapshot.value() != expected
        || !handle->search() || !handle->recommendation() || !handle->documents())
        return false;
    const auto *record = handle->document(base::DocumentId::from_integer(77).value());
    return record && record->content == std::string("body\0\xff", 6)
           && handle->documents()->size() == 2;
}
} // namespace

// GNU ld只在本测试包装Reader::open和标量new：Reader真实完成后精确注入Manager分配失败。
extern "C" base::Result<snapshot::SnapshotReader>
real_open(const std::string &, base::SnapshotVersion, const snapshot::SnapshotReadLimits &) __asm__(
    "__real__ZN8siftwing8snapshot14SnapshotReader4openERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS_4base6detail8StrongIdINSB_18SnapshotVersionTagEEERKNS0_18SnapshotReadLimitsE");
extern "C" base::Result<snapshot::SnapshotReader> wrapped_open(const std::string &parent,
    base::SnapshotVersion requested,
    const snapshot::SnapshotReadLimits &
        budget) __asm__("__wrap__ZN8siftwing8snapshot14SnapshotReader4openERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS_4base6detail8StrongIdINSB_18SnapshotVersionTagEEERKNS0_18SnapshotReadLimitsE");
extern "C" base::Result<snapshot::SnapshotReader> wrapped_open(const std::string &parent,
    base::SnapshotVersion requested,
    const snapshot::SnapshotReadLimits &budget) {
    auto result = real_open(parent, requested, budget);
    remaining = fail_after_open;
    fail_after_open = -1;
    return result;
}
extern "C" void *real_new(std::size_t) __asm__("__real__Znwm");
extern "C" void *wrapped_new(std::size_t) __asm__("__wrap__Znwm");
extern "C" void *wrapped_new(std::size_t size) {
    if (remaining == 0) {
        remaining = -1;
        throw std::bad_alloc();
    }
    if (remaining > 0)
        --remaining;
    return real_new(size);
}

static_assert(!std::is_copy_constructible_v<snapshot::SnapshotManager>);
static_assert(!std::is_move_constructible_v<snapshot::SnapshotManager>);
static_assert(
    std::is_same_v<snapshot::SnapshotHandle::element_type, const snapshot::SnapshotReader>);

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    try {
        const std::string parent = argv[1];
        const std::string bad = argv[2];
        const auto max = std::numeric_limits<std::uint64_t>::max();
        const auto budget = limits();
        auto opened = snapshot::SnapshotManager::open(parent, version(0), budget);
        require(bool(opened), "initial open");
        auto manager = std::move(opened).value();
        auto current = manager->current();
        auto first = manager->last_load();
        require(valid(current, 0), "current data/version");
        require(first && first->requested_version == 0 && first->outcome
                    && first->outcome.value() == current,
            "initial report identity");
        std::weak_ptr<const snapshot::SnapshotReader> weak_current = current;
        auto candidate = manager->load_candidate(parent, version(max), budget);
        require(candidate == manager->last_load() && candidate->requested_version == max
                    && candidate->outcome && valid(candidate->outcome.value(), max),
            "candidate complete");
        require(candidate->outcome.value() != current && manager->current() == current,
            "candidate success never publishes");
        std::weak_ptr<const snapshot::SnapshotReader> weak_candidate = candidate->outcome.value();
        // 独立损坏输入必须通过同一个完整Reader校验；错误上下文和文本均保持。
        const auto rejected = snapshot::SnapshotReader::open(bad, version(1), budget);
        require(!rejected, "invalid fixture rejected independently");
        const auto failure = manager->load_candidate(bad, version(1), budget);
        require(failure == manager->last_load() && failure->requested_version == 1
                    && !failure->outcome
                    && failure->outcome.error().context == rejected.error().context
                    && failure->outcome.error().message == rejected.error().message,
            "diagnostic preserved");
        require(manager->current() == current && valid(candidate->outcome.value(), max),
            "failure preserves current and held previous candidate");
        const auto bad_initial = snapshot::SnapshotManager::open(bad, version(1), budget);
        require(!bad_initial && bad_initial.error().context == rejected.error().context,
            "initial failure returns no manager");
        auto invalid_limits = budget;
        invalid_limits.documents.max_documents = 0;
        const auto limited = manager->load_candidate(parent, version(0), invalid_limits);
        require(!limited->outcome && limited->outcome.error().context == "reader.limits"
                    && manager->current() == current,
            "invalid limits never replace current");
        const auto empty = manager->load_candidate(parent, version(2), budget);
        require(empty->outcome && empty->outcome.value()->manifest()->snapshot.value() == 2
                    && empty->outcome.value()->documents()->empty()
                    && manager->current() == current,
            "empty snapshot is valid candidate");
        // 分别拒绝共享Reader、报告、初始Manager的分配；不将异常伪装成加载失败。
        for (int allocation = 0; allocation != 2; ++allocation) {
            const auto before = manager->last_load();
            fail_after_open = allocation;
            bool threw = false;
            try {
                (void)manager->load_candidate(parent, version(max), budget);
            } catch (const std::bad_alloc &) {
                threw = true;
            }
            remaining = -1;
            fail_after_open = -1;
            require(threw && manager->last_load() == before && manager->current() == current,
                "candidate allocation exception has strong state guarantee");
        }
        for (int allocation = 0; allocation != 3; ++allocation) {
            fail_after_open = allocation;
            bool threw = false;
            try {
                (void)snapshot::SnapshotManager::open(parent, version(0), budget);
            } catch (const std::bad_alloc &) {
                threw = true;
            }
            remaining = -1;
            fail_after_open = -1;
            require(threw, "initial allocation exception propagates");
        }
        const auto before = manager->last_load();
        fail_after_open = 0;
        bool threw = false;
        try {
            (void)manager->load_candidate(bad, version(1), budget);
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        require(
            threw && manager->last_load() == before, "failure report allocation preserves last");
        // current只读成员与控制线程last替换互不接触；异常时也须join再析构Manager。
        std::atomic<bool> start{false}, stop{false}, good{true};
        std::atomic<unsigned> reads{0};
        std::vector<std::thread> readers;
        const auto join = [&] {
            stop.store(true);
            start.store(true);
            for (auto &thread : readers)
                thread.join();
        };
        try {
            for (unsigned i = 0; i != 8; ++i)
                readers.emplace_back([&] {
                    while (!start.load())
                        std::this_thread::yield();
                    unsigned count = 0;
                    do {
                        const auto handle = manager->current();
                        if (!valid(handle, 0) || handle != current)
                            good.store(false);
                        ++count;
                    } while (!stop.load() || count < 1000);
                    reads.fetch_add(count);
                });
            start.store(true);
            for (unsigned i = 0; i != 20; ++i) {
                (void)manager->load_candidate(parent, version(max), budget);
                (void)manager->load_candidate(bad, version(1), budget);
            }
        } catch (...) {
            join();
            throw;
        }
        join();
        require(good.load() && reads.load() >= 8000, "concurrent current stays complete and fixed");
        // 不以use_count具体数字为合同：只验证最后所有者释放前后弱引用的可观察寿命。
        candidate.reset();
        require(weak_candidate.expired(), "unheld replaced candidate released");
        auto survivor = manager->load_candidate(parent, version(max), budget);
        weak_candidate = survivor->outcome.value();
        manager.reset();
        require(
            valid(current, 0) && valid(survivor->outcome.value(), max), "handles survive manager");
        current.reset();
        require(!weak_current.expired(), "initial report retains reader");
        first.reset();
        require(weak_current.expired(), "last current owner releases reader");
        survivor.reset();
        require(weak_candidate.expired(), "last candidate owner releases reader");
        std::cout << "snapshot manager checks=" << checks << " reads=" << reads.load() << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
