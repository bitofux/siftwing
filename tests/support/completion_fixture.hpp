/*
 * PROJECT : SIFTWING
 * FILE    : completion_fixture.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 原创结果回投测试的捕获提交器、拥有载荷与本地socket入口
 */
#pragma once
#include "connection_harness.hpp"
#include "siftwing/concurrency/worker_pool.hpp"
#include <fcntl.h>

namespace siftwing::test {
using namespace concurrency;
/** @brief 仅测试用捕获接口；以真实同步队列跨线程交接，不执行生产查询。 */
struct CaptureSubmitter final : RoutedRequestSubmitter {
    TaskQueue<RoutedSearchTask> requests{1024};
    TaskSubmitResult refusal = TaskSubmitResult::accepted; // start前设置，运行期间冻结。
    TaskSubmitResult try_submit(std::unique_ptr<RoutedSearchTask> &source) override {
        return refusal == TaskSubmitResult::accepted ? requests.try_submit(source) : refusal;
    }
};
inline std::unique_ptr<RoutedSearchTask> take_request(CaptureSubmitter &sink) {
    std::unique_ptr<RoutedSearchTask> task;
    await([&] {
        task = sink.requests.try_pop();
        return static_cast<bool>(task);
    });
    return task;
}
inline std::unique_ptr<RoutedSearchTask> fabricated_task(
    ConnectionId route, std::uint64_t id = 77, MessageType type = MessageType::search_request) {
    Frame frame{type, 0, id, "{}"};
    auto payload = std::get<SearchTask>(SearchTask::create(frame, 128));
    auto routed = RoutedSearchTask::create(route, payload);
    return std::make_unique<RoutedSearchTask>(std::get<RoutedSearchTask>(std::move(routed)));
}
inline std::unique_ptr<RoutedCompletion> completion(
    const RoutedSearchTask &task, std::string_view body = "{\"test\":true}") {
    auto result = RoutedCompletion::success(task, body);
    return std::make_unique<RoutedCompletion>(std::get<RoutedCompletion>(std::move(result)));
}
inline UniqueFd attach(ConnectionHarness &sub, int reserved_number = -1) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair))
        throw std::system_error(errno, std::generic_category());
    UniqueFd source(pair[0]), peer(pair[1]);
    if (reserved_number >= 0 && source.get() != reserved_number) {
        if (peer.get() == reserved_number)
            throw std::runtime_error("reserved peer alias");
        if (::dup3(source.get(), reserved_number, O_CLOEXEC) != reserved_number)
            throw std::system_error(errno, std::generic_category());
        (void)source.close();
        source = UniqueFd(reserved_number);
    }
    if (sub.adopt(source))
        throw std::runtime_error("test source refused");
    return peer;
}
inline void publish(CompletionTarget target, std::unique_ptr<RoutedCompletion> result) {
    const auto receipt = try_publish_and_notify(*target.queue, result, target.wakeup_fd);
    if (receipt.publish != CompletionPublishResult::accepted || receipt.notification_error
        || result)
        throw std::runtime_error("test completion refused");
}
inline void stop_core(ConnectionHarness &sub) {
    if (const auto error = sub.request_stop())
        throw std::system_error(error);
    // 调用者在此函数前已等待所有外部完整发布及最后通知返回。
    sub.release_publishers();
    if (const auto error = sub.join())
        throw std::system_error(error);
}
} // namespace siftwing::test
