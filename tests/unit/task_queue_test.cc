/*
 * PROJECT : SIFTWING
 * FILE    : task_queue_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 独立FIFO参考、源保留、关闭排空与锁外任务销毁
 */
#include "siftwing/concurrency/task_queue.hpp"
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>

namespace {
using namespace siftwing::concurrency;
unsigned int checks = 0, destroyed = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << "task queue failed: " << label << '\n';
        std::exit(1);
    }
}
struct Job {
    unsigned int id;
    void *context = nullptr;
    void (*on_destroy)(void *) noexcept = nullptr;
    explicit Job(unsigned int value) : id(value) {}
    ~Job() noexcept {
        ++destroyed;
        if (on_destroy)
            on_destroy(context);
    }
    Job(const Job &) = delete;
    Job(Job &&) = delete;
};
using Queue = TaskQueue<Job>;
static_assert(!std::is_copy_constructible_v<Queue> && !std::is_move_constructible_v<Queue>);
static_assert(!std::is_copy_assignable_v<Queue> && !std::is_move_assignable_v<Queue>);
static_assert(!std::is_invocable_v<decltype(&Queue::try_submit), Queue &, std::unique_ptr<Job> &&>);
static_assert(std::is_same_v<decltype(std::declval<Queue &>().wait_pop()), std::unique_ptr<Job>>);
void reenter(void *context) noexcept {
    auto &queue = *static_cast<Queue *>(context);
    require(queue.status().size == 0, "consumer destroys outside queue lock");
}
} // namespace
int main() {
    bool rejected = false;
    try {
        Queue queue(0);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "zero capacity");
    rejected = false;
    try {
        Queue queue(std::numeric_limits<std::size_t>::max());
    } catch (const std::length_error &) {
        rejected = true;
    }
    require(rejected, "unrepresentable capacity before allocation");
    unsigned int serial = 0;
    // 参考按成功提交写入deque；没有复用生产head/tail计算或槽索引。
    for (const std::size_t capacity : {1U, 3U, 7U}) {
        Queue queue(capacity);
        std::deque<unsigned int> expected;
        std::unique_ptr<Job> empty;
        require(!queue.try_pop(), "open empty pop");
        require(queue.try_submit(empty) == TaskSubmitResult::invalid_task, "open null source");
        for (unsigned int round = 0; round < 80; ++round) {
            while (expected.size() < capacity) {
                auto source = std::make_unique<Job>(++serial);
                const auto id = source->id;
                require(queue.try_submit(source) == TaskSubmitResult::accepted && !source,
                    "accepted moves exact owner");
                expected.push_back(id);
            }
            auto source = std::make_unique<Job>(++serial);
            auto *identity = source.get();
            require(queue.try_submit(source) == TaskSubmitResult::full && source.get() == identity,
                "full preserves exact task");
            require(
                queue.try_submit(empty) == TaskSubmitResult::invalid_task, "invalid before full");
            auto first = queue.try_pop();
            require(first && first->id == expected.front(), "FIFO head");
            expected.pop_front();
            require(queue.try_submit(source) == TaskSubmitResult::accepted && !source,
                "refill across ring tail");
            expected.push_back(identity->id);
            require(queue.status().capacity == capacity && queue.status().size == expected.size(),
                "fixed bounded status");
            while (!expected.empty()) {
                auto item = queue.try_pop();
                require(item && item->id == expected.front(), "independent FIFO order");
                expected.pop_front();
            }
            require(!queue.try_pop(), "empty after drain");
        }
        // 通知先于wait_pop也必须由真实谓词读取任务；close保留条目而非取消。
        auto source = std::make_unique<Job>(++serial);
        const auto id = source->id;
        require(queue.try_submit(source) == TaskSubmitResult::accepted, "enqueue before wait");
        queue.close();
        queue.close();
        source = std::make_unique<Job>(++serial);
        auto *identity = source.get();
        require(queue.try_submit(source) == TaskSubmitResult::closed && source.get() == identity,
            "closed retains source");
        require(queue.try_submit(empty) == TaskSubmitResult::closed, "closed before invalid");
        require(queue.status().closed && queue.status().size == 1, "close keeps queued task");
        auto item = queue.wait_pop();
        require(item && item->id == id, "drain after close");
        require(!queue.wait_pop() && !queue.try_pop(), "closed empty terminates wait");
    }
    {
        Queue queue(1);
        auto source = std::make_unique<Job>(++serial);
        source->context = &queue;
        source->on_destroy = &reenter;
        const auto before = destroyed;
        require(queue.try_submit(source) == TaskSubmitResult::accepted, "hook task accepted");
        auto item = queue.wait_pop();
        require(destroyed == before, "submit/pop never destroy user task");
        item.reset();
        queue.close();
        require(destroyed == before + 1, "consumer performs sole destruction");
    }
    const auto before = destroyed;
    {
        Queue queue(2);
        auto source = std::make_unique<Job>(++serial);
        require(queue.try_submit(source) == TaskSubmitResult::accepted, "residual ownership");
        queue.close();
        require(destroyed == before, "close does not destroy residual task");
    }
    require(destroyed == before + 1, "inactive queue destructor fallback");
    std::cout << "task queue checks=" << checks << '\n';
}
