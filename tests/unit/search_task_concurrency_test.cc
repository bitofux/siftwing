/*
 * PROJECT : SIFTWING
 * FILE    : search_task_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 测试专属WorkerTask包装下的拥有载荷交接和输入/任务先销毁
 */
#include "siftwing/concurrency/completion.hpp"
#include "siftwing/concurrency/worker_pool.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>

namespace {
using namespace siftwing::concurrency;
using namespace siftwing::net;
constexpr std::size_t count = 1024;
unsigned int checks = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}
struct Job final : WorkerTask {
    SearchTask task;
    TaskQueue<Completion> &outputs; // 测试中控制者保持到pool.join，不是生产CompletionQueue合同。
    std::atomic<unsigned int> &bad;
    Job(SearchTask source, TaskQueue<Completion> &queue, std::atomic<unsigned int> &errors)
        : task(std::move(source)), outputs(queue), bad(errors) {}
    void execute() override {
        const auto *request = task.request();
        if (!request
            || request->body != "{\"number\":" + std::to_string(request->request_id) + "}") {
            ++bad;
            return;
        }
        // 响应在本任务销毁前转交；消费者有时在任务之后才取得结果。
        auto result =
            Completion::success(task, "{\"result\":" + std::to_string(request->request_id) + "}");
        if (!std::holds_alternative<Completion>(result)) {
            ++bad;
            return;
        }
        auto output = std::make_unique<Completion>(std::get<Completion>(std::move(result)));
        if (outputs.try_submit(output) != TaskSubmitResult::accepted)
            ++bad;
    }
};
}
int main() {
    for (unsigned int round = 0; round < 6; ++round) {
        TaskQueue<Completion> outputs(count);
        WorkerPool pool(3, 7);
        std::atomic<unsigned int> bad{0};
        std::array<std::thread, 4> producers;
        for (std::size_t p = 0; p < producers.size(); ++p)
            producers[p] = std::thread([&, p] {
                for (std::size_t i = p * 256 + 1; i <= (p + 1) * 256; ++i) {
                    Frame source{
                        i % 2 == 0 ? MessageType::search_request : MessageType::suggest_request,
                        0,
                        i,
                        "{\"number\":" + std::to_string(i) + "}"};
                    auto result = SearchTask::create(source, 128);
                    if (!std::holds_alternative<SearchTask>(result) || source.request_id != 0
                        || !source.body.empty()) {
                        ++bad;
                        continue;
                    }
                    std::unique_ptr<WorkerTask> job = std::make_unique<Job>(
                        std::get<SearchTask>(std::move(result)), outputs, bad);
                    // 移走拥有载荷后立即改写源，再发布；源存储不应影响worker。
                    source.body.assign(256, 'x');
                    source.request_id = 9999;
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
        pool.join(); // 所有源/Job已销毁，再消费完整结果，直接证明不借用旧对象。
        outputs.close();
        std::array<unsigned int, count> observed{};
        while (auto output = outputs.wait_pop()) {
            const auto *frame = output->frame();
            require(
                frame && frame->request_id >= 1 && frame->request_id <= count, "owned result id");
            const auto id = frame->request_id;
            ++observed[id - 1];
            require(frame->body == "{\"result\":" + std::to_string(id) + "}" && frame->status == 0,
                "result independent of destroyed task");
            require(
                frame->message_type
                    == (id % 2 == 0 ? MessageType::search_response : MessageType::suggest_response),
                "result request type correlated");
        }
        for (const auto value : observed)
            require(value == 1, "every independent producer id exactly once");
        require(bad == 0 && pool.report().succeeded == count && pool.report().failed == 0,
            "handoff final report");
    }
    std::cout << "search handoff checks=" << checks << '\n';
}
