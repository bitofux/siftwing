/*
 * PROJECT : SIFTWING
 * FILE    : completion_wakeup.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 提交成功才通知，通知失败保留独占结果交接事实
 */
#include "siftwing/net/completion_wakeup.hpp"
namespace siftwing::net {
CompletionWakeupResult try_publish_and_notify(concurrency::CompletionQueue &queue,
    std::unique_ptr<concurrency::RoutedCompletion> &source,
    int stable_wakeup_fd) {
    const auto result = queue.try_publish(source);
    // 提交已经完成且锁已释放；消费者可先于本次通知取出，不按通知成败回滚。
    return {result,
        result == concurrency::CompletionPublishResult::accepted ? notify_eventfd(stable_wakeup_fd)
                                                                 : std::error_code{}};
}
} // namespace siftwing::net
