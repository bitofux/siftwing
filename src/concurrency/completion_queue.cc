/*
 * PROJECT : SIFTWING
 * FILE    : completion_queue.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 先拒绝后提交的固定结果环，归属检查与关闭保留
 */
#include "siftwing/concurrency/completion_queue.hpp"
#include <stdexcept>
#include <utility>
namespace siftwing::concurrency {
namespace {
std::size_t checked(std::size_t capacity) {
    if (capacity == 0 || capacity > 65536)
        throw std::invalid_argument("invalid completion queue capacity");
    return capacity;
}
}
CompletionQueue::CompletionQueue(std::uint32_t reactor, std::size_t capacity)
    : reactor_id_(reactor), slots_(checked(capacity)) {}
CompletionPublishResult CompletionQueue::try_publish(std::unique_ptr<RoutedCompletion> &source) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
        return CompletionPublishResult::closed;
    if (!source || !source->valid())
        return CompletionPublishResult::invalid_completion;
    if (source->route().reactor_id != reactor_id_)
        return CompletionPublishResult::wrong_reactor;
    if (size_ == slots_.size())
        return CompletionPublishResult::full;
    // 环区之外为空；距离环尾的计算避免head+size溢出，提交只移动unique_ptr。
    const auto gap = slots_.size() - head_;
    const auto tail = size_ >= gap ? size_ - gap : head_ + size_;
    slots_[tail] = std::move(source);
    ++size_;
    return CompletionPublishResult::accepted;
}
std::unique_ptr<RoutedCompletion> CompletionQueue::try_pop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (size_ == 0)
        return {};
    auto output = std::move(slots_[head_]);
    ++head_;
    if (head_ == slots_.size())
        head_ = 0;
    --size_;
    return output; // 拥有者在锁外检查/销毁，锁内不执行业务或访问身份表。
}
void CompletionQueue::close() {
    const std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}
CompletionQueueStatus CompletionQueue::status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return {reactor_id_, slots_.size(), size_, closed_};
}
} // namespace siftwing::concurrency
