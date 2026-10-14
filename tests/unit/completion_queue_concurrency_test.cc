/*
 * PROJECT : SIFTWING
 * FILE    : completion_queue_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 真实worker结果发布及关闭竞争的独立恰一次参考
 */
#include "siftwing/concurrency/completion_queue.hpp"
#include "siftwing/concurrency/worker_pool.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
namespace {
using namespace siftwing::concurrency;
using namespace siftwing::net;
constexpr std::size_t count = 512;
unsigned int checks = 0;
void require(bool value, const char *why) {
    ++checks;
    if (!value) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
std::unique_ptr<RoutedCompletion> output(std::uint64_t id) {
    Frame frame{MessageType::search_request, 0, id, "{}"};
    auto source_result = SearchTask::create(frame, 128);
    auto source = std::get<SearchTask>(std::move(source_result));
    auto task_result =
        RoutedSearchTask::create({31, static_cast<std::uint32_t>(id % 32), id}, source);
    auto task = std::get<RoutedSearchTask>(std::move(task_result));
    auto result = RoutedCompletion::success(task, "{\"result\":" + std::to_string(id) + "}");
    return std::make_unique<RoutedCompletion>(std::get<RoutedCompletion>(std::move(result)));
}
struct Job final : WorkerTask {
    std::unique_ptr<RoutedCompletion> value;
    CompletionQueue &queue;
    std::atomic<unsigned int> &bad;
    Job(std::unique_ptr<RoutedCompletion> source,
        CompletionQueue &target,
        std::atomic<unsigned int> &errors)
        : value(std::move(source)), queue(target), bad(errors) {}
    void execute() override {
        // 本实验主动重试以验证持续消费；不规定生产worker在full时等待或忙循环。
        auto published = queue.try_publish(value);
        while (published == CompletionPublishResult::full) {
            std::this_thread::yield();
            published = queue.try_publish(value);
        }
        if (published != CompletionPublishResult::accepted || value)
            ++bad;
    }
};
}
int main() {
    for (unsigned int round = 0; round < 6; ++round) {
        CompletionQueue queue(31, 7);
        WorkerPool pool(3, 7);
        std::atomic<unsigned int> bad{0};
        std::array<unsigned int, count> observed{};
        std::thread consumer([&] {
            for (;;) {
                auto value = queue.try_pop();
                if (!value) {
                    const auto state = queue.status();
                    // 空pop与status之间仍可能有最后发布，必须同锁观测closed且empty。
                    if (state.closed && state.size == 0)
                        break;
                    std::this_thread::yield();
                    continue;
                }
                const auto *frame = value->payload()->frame();
                const auto id = frame->request_id;
                if (id < 1 || id > count) {
                    ++bad;
                    continue;
                }
                ++observed[id - 1];
                if (value->route() != ConnectionId{31, static_cast<std::uint32_t>(id % 32), id}
                    || frame->message_type != MessageType::search_response || frame->status != 0
                    || frame->body != "{\"result\":" + std::to_string(id) + "}")
                    ++bad;
            }
        });
        std::array<std::thread, 4> producers;
        for (std::size_t p = 0; p < producers.size(); ++p)
            producers[p] = std::thread([&, p] {
                for (std::size_t id = p * 128 + 1; id <= (p + 1) * 128; ++id) {
                    std::unique_ptr<WorkerTask> job = std::make_unique<Job>(output(id), queue, bad);
                    auto submitted = pool.try_submit(job);
                    while (submitted == TaskSubmitResult::full) {
                        std::this_thread::yield();
                        submitted = pool.try_submit(job);
                    }
                    if (submitted != TaskSubmitResult::accepted || job)
                        ++bad;
                }
            });
        for (auto &producer : producers)
            producer.join();
        pool.join();
        queue.close();
        consumer.join();
        for (const auto value : observed)
            require(value == 1, "every worker result exactly once");
        require(bad == 0 && pool.report().succeeded == count && pool.report().failed == 0
                    && queue.status().closed && queue.status().size == 0,
            "producer/worker/consumer shutdown ordered");
    }
    // 发布和close同锁排序；独立记录每次结论，不依赖谁赢或任何完成顺序。
    for (unsigned int round = 0; round < 16; ++round) {
        CompletionQueue queue(31, 1024);
        std::atomic<unsigned int> ready{0}, bad{0};
        std::atomic<bool> go{false};
        std::array<bool, count> accepted{};
        std::array<std::thread, 8> producers;
        for (std::size_t p = 0; p < producers.size(); ++p)
            producers[p] = std::thread([&, p] {
                auto first = output(p * 64 + 1);
                ++ready;
                while (!go.load())
                    std::this_thread::yield();
                for (std::size_t id = p * 64 + 1; id <= (p + 1) * 64; ++id) {
                    auto value = id == p * 64 + 1 ? std::move(first) : output(id);
                    const auto *pointer = value.get();
                    const auto result = queue.try_publish(value);
                    if (result == CompletionPublishResult::accepted) {
                        accepted[id - 1] = true;
                        if (value)
                            ++bad;
                    } else if (result != CompletionPublishResult::closed || value.get() != pointer
                               || value->payload()->frame()->request_id != id)
                        ++bad;
                }
            });
        while (ready.load() != 8)
            std::this_thread::yield();
        go.store(true);
        queue.close();
        for (auto &producer : producers)
            producer.join();
        std::array<unsigned int, count> observed{};
        while (auto value = queue.try_pop()) {
            const auto id = value->payload()->frame()->request_id;
            require(id >= 1 && id <= count, "closed race valid id");
            ++observed[id - 1];
        }
        for (std::size_t i = 0; i < count; ++i)
            require(
                observed[i] == (accepted[i] ? 1U : 0U), "close race independent receipt oracle");
        require(bad == 0 && queue.status().size == 0 && queue.status().closed,
            "closed refusal preserves every nonaccepted owner");
    }
    std::cout << "completion concurrency checks=" << checks << '\n';
}
