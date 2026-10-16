/*
 * PROJECT : SIFTWING
 * FILE    : snapshot_loader_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 用真实Reader及确定门禁证明后台隔离、单槽、异常恢复和停止排空
 */
#include "siftwing/snapshot/loader.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <system_error>
#include <type_traits>
#include <vector>

using namespace siftwing;
namespace {
using Submit = snapshot::SnapshotLoadSubmit;
using Completion = snapshot::SnapshotLoadCompletion;
unsigned checks = 0;
thread_local int remaining = -1;
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool armed = false, entered = false, release = false;
    int allocation = -1, exception = 0;
    std::thread::id loader_thread;
} gate;

void require(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        std::cerr << label << '\n';
        std::exit(1); // 失败进程退出，避免测试本身在尚未释放的故障门禁上析构等待。
    }
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
void arm(int allocation = -1, int exception = 0) {
    const std::lock_guard<std::mutex> lock(gate.mutex);
    gate.armed = true;
    gate.entered = false;
    gate.release = false;
    gate.allocation = allocation;
    gate.exception = exception;
}
void entered() {
    std::unique_lock<std::mutex> lock(gate.mutex);
    require(gate.cv.wait_for(lock,
                std::chrono::seconds(5),
                [] {
                    return gate.entered;
                }),
        "background reached Reader gate");
    require(gate.loader_thread != std::this_thread::get_id(), "Reader runs off submit thread");
}
void release() {
    {
        const std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.cv.notify_one();
}
Completion completed(snapshot::SnapshotLoader &loader) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        auto result = loader.take_completed();
        if (result)
            return std::move(*result);
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    require(false, "completion deadline");
    std::abort();
}
} // namespace

// GNU ld包装仅测试：门禁后仍调用真实Reader；Reader返回后拒绝共享Reader/报告分配。
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
    int allocation = -1, exception = 0;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (gate.armed) {
            gate.armed = false;
            gate.entered = true;
            gate.loader_thread = std::this_thread::get_id();
            gate.cv.notify_one();
            gate.cv.wait(lock, [] {
                return gate.release;
            });
            allocation = gate.allocation;
            exception = gate.exception;
        }
    }
    if (exception == 1)
        throw std::bad_alloc();
    if (exception == 2)
        throw 77; // 非std::exception同样必须隔离，禁止后台线程异常逸出。
    auto result = real_open(parent, requested, budget);
    remaining = allocation;
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
static_assert(!std::is_copy_constructible_v<snapshot::SnapshotLoader>);
static_assert(!std::is_move_constructible_v<snapshot::SnapshotLoader>);

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    const std::string parent = argv[1], bad = argv[2];
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const auto budget = limits();
    auto opened = snapshot::SnapshotManager::open(parent, version(0), budget);
    require(bool(opened), "initial open");
    auto manager = std::move(opened).value();
    auto current = manager->current();
    auto previous = manager->last_load();
    const auto rejected = snapshot::SnapshotReader::open(bad, version(1), budget);
    require(!rejected, "independent corrupt input rejected");
    const auto request = [&](std::uint64_t value) {
        return snapshot::SnapshotLoadRequest{parent, version(value), budget};
    };
    // 未返回Loader不能接受任务或改Manager；覆盖启动前线程状态分配失败。
    remaining = 0;
    bool allocation_failed = false;
    try {
        snapshot::SnapshotLoader loader(*manager);
    } catch (const std::bad_alloc &) {
        allocation_failed = true;
    }
    remaining = -1;
    require(
        allocation_failed && manager->last_load() == previous, "startup allocation strong state");
    snapshot::SnapshotHandle held_candidate;
    {
        snapshot::SnapshotLoader loader(*manager);
        require(!loader.take_completed(), "initial completion empty");
        arm();
        auto owned_input = request(max);
        require(loader.try_submit(owned_input) == Submit::accepted,
            "submit returns before Reader releases");
        owned_input.parent_path = "discarded source path";
        entered();
        require(!loader.take_completed() && loader.try_submit(request(0)) == Submit::busy,
            "loading occupies single slot and no partial completion");
        std::atomic<bool> good{true};
        std::vector<std::thread> readers;
        for (unsigned i = 0; i != 8; ++i)
            readers.emplace_back([&] {
                for (unsigned n = 0; n != 1000; ++n) {
                    const auto handle = manager->current();
                    if (handle != current || !valid(handle, 0))
                        good.store(false);
                }
            });
        for (auto &reader : readers)
            reader.join();
        require(good.load(), "8000 old snapshot reads finish while Reader remains gated");
        // 多生产者竞争同一已占槽；一个结果未取走前不能再接受任何候选。
        std::atomic<unsigned> busy{0};
        std::vector<std::thread> producers;
        for (unsigned i = 0; i != 8; ++i)
            producers.emplace_back([&] {
                if (loader.try_submit(request(0)) == Submit::busy)
                    busy.fetch_add(1);
            });
        for (auto &producer : producers)
            producer.join();
        require(busy.load() == 8, "concurrent producers refused without overwrite");
        release();
        auto result = completed(loader);
        require(result.requested_version == max && result.report && !result.exception
                    && result.report->outcome && valid(result.report->outcome.value(), max),
            "owned input and complete candidate delivered");
        held_candidate = result.report->outcome.value();
        previous = result.report;
        require(manager->current() == current && !loader.take_completed(),
            "success never publishes or repeats");
        require(loader.try_submit({bad, version(1), budget}) == Submit::accepted,
            "slot reusable after take");
        auto failure = completed(loader);
        require(failure.requested_version == 1 && failure.report && !failure.exception
                    && !failure.report->outcome
                    && failure.report->outcome.error().context == rejected.error().context
                    && failure.report->outcome.error().message == rejected.error().message,
            "ordinary failure preserves original diagnostic");
        previous = failure.report;
        auto invalid = budget;
        invalid.documents.max_documents = 0;
        require(loader.try_submit({parent, version(0), invalid}) == Submit::accepted,
            "limits validated asynchronously");
        auto limited = completed(loader);
        require(limited.report && !limited.exception && !limited.report->outcome
                    && limited.report->outcome.error().context == "reader.limits",
            "complete Reader budget validation");
        previous = limited.report;
        // 真Reader完成后的两次分配和Reader内部异常均交付exception_ptr，旧last保持。
        for (int scenario = 0; scenario != 4; ++scenario) {
            arm(scenario < 2 ? scenario : -1, scenario < 2 ? 0 : scenario - 1);
            require(
                loader.try_submit(request(max)) == Submit::accepted, "exception attempt accepted");
            entered();
            release();
            auto thrown = completed(loader);
            require(thrown.requested_version == max && !thrown.report && bool(thrown.exception),
                "exception owns exact request identity and no partial report");
            bool original = false;
            try {
                std::rethrow_exception(thrown.exception);
            } catch (const std::bad_alloc &) {
                original = scenario != 3;
            } catch (int value) {
                original = scenario == 3 && value == 77;
            }
            require(original && manager->current() == current,
                "original exception crosses thread as value");
        }
        loader.join();
        require(manager->last_load() == previous, "all exceptions preserve original last report");
        require(loader.try_submit(request(0)) == Submit::stopped, "closed refuses further load");
        loader.join();
    }
    // 后续Loader恢复成功，含空候选、相同/回退版本；关闭时排空已接受任务并保留完成值。
    {
        snapshot::SnapshotLoader loader(*manager);
        require(
            loader.try_submit(request(2)) == Submit::accepted, "restart with separate new loader");
        auto empty = completed(loader);
        require(empty.report && empty.report->outcome && !empty.exception
                    && empty.report->outcome.value()->documents()->empty(),
            "empty candidate valid");
        require(
            loader.try_submit(request(0)) == Submit::accepted, "same/backward version accepted");
        auto same = completed(loader);
        require(same.report && same.report->outcome && valid(same.report->outcome.value(), 0),
            "same version still completely loaded");
        arm();
        require(loader.try_submit(request(max)) == Submit::accepted, "stop test accepted");
        entered();
        std::thread stopper([&] {
            loader.request_stop();
            loader.request_stop();
        });
        stopper.join();
        require(loader.try_submit(request(0)) == Submit::stopped && !loader.take_completed(),
            "stop returns during load, prioritizes stopped and does not invent completion");
        release();
        loader.join();
        require(loader.try_submit(request(0)) == Submit::stopped,
            "unconsumed result never reopens closed loader");
        auto drained = loader.take_completed();
        require(drained && drained->report && drained->report->outcome && !drained->exception
                    && valid(drained->report->outcome.value(), max) && !loader.take_completed(),
            "join drains accepted task, result remains exactly once");
    }
    // 析构不要求先取结果，等待中的输入也必须执行完；join后Manager恢复控制者串行访问。
    {
        snapshot::SnapshotLoader loader(*manager);
        require(loader.try_submit(request(2)) == Submit::accepted, "destructor drain accepted");
    }
    require(manager->last_load()->requested_version == 2 && manager->last_load()->outcome
                && manager->current() == current,
        "destructor drains and never publishes");
    {
        snapshot::SnapshotLoader loader(*manager);
        arm();
        std::atomic<bool> start{false};
        std::atomic<unsigned> accepted{0}, busy{0}, taken{0};
        std::vector<std::thread> producers;
        for (unsigned i = 0; i != 8; ++i)
            producers.emplace_back([&] {
                while (!start.load())
                    std::this_thread::yield();
                const auto outcome = loader.try_submit(request(max));
                if (outcome == Submit::accepted)
                    accepted.fetch_add(1);
                if (outcome == Submit::busy)
                    busy.fetch_add(1);
            });
        start.store(true);
        for (auto &producer : producers)
            producer.join();
        entered();
        require(accepted.load() == 1 && busy.load() == 7,
            "concurrent idle submission accepts exactly one owned request");
        release();
        loader.join();
        std::vector<std::thread> consumers;
        for (unsigned i = 0; i != 8; ++i)
            consumers.emplace_back([&] {
                if (auto result = loader.take_completed()) {
                    if (result->report && result->report->outcome && !result->exception
                        && valid(result->report->outcome.value(), max))
                        taken.fetch_add(1);
                }
            });
        for (auto &consumer : consumers)
            consumer.join();
        require(taken.load() == 1, "concurrent consumers obtain completed result exactly once");
    }
    {
        snapshot::SnapshotLoader idle(*manager);
        idle.join();
        require(!idle.take_completed(), "idle stop wakes with no phantom result");
    }
    std::weak_ptr<const snapshot::SnapshotReader> weak = held_candidate;
    previous.reset();
    manager.reset();
    require(valid(held_candidate, max) && valid(current, 0),
        "held candidate/current outlive loader and manager");
    held_candidate.reset();
    require(weak.expired(), "last candidate owner releases complete Reader");
    std::cout << "loader checks=" << checks << " old_reads=8000\n";
}
