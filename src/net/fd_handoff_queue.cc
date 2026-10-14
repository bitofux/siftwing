/*
 * PROJECT : SIFTWING
 * FILE    : fd_handoff_queue.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 预分配环形槽位的无分配所有权交接
 */
#include "siftwing/net/fd_handoff_queue.hpp"
#include <stdexcept>
#include <utility>

namespace siftwing::net {
FdHandoffQueue::FdHandoffQueue(std::size_t capacity) {
    if (capacity == 0)
        throw std::invalid_argument("fd handoff capacity must be positive");
    slots_.resize(capacity);
}

FdHandoffResult FdHandoffQueue::try_push(UniqueFd &source) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
        return FdHandoffResult::closed;
    if (!source)
        return FdHandoffResult::invalid_fd;
    if (size_ == slots_.size())
        return FdHandoffResult::full;
    // 用距环尾的距离避免head_ + size_溢出；目标槽为空，移动赋值不会系统close。
    const auto gap = slots_.size() - head_;
    const auto tail = size_ >= gap ? size_ - gap : head_ + size_;
    slots_[tail] = std::move(source);
    ++size_;
    return FdHandoffResult::accepted;
}

std::optional<UniqueFd> FdHandoffQueue::try_pop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (size_ == 0)
        return std::nullopt;
    // 在锁内仅move；返回拥有值后锁已释放，调用方才可注册/关闭资源。
    std::optional<UniqueFd> result(std::in_place, std::move(slots_[head_]));
    ++head_;
    if (head_ == slots_.size())
        head_ = 0;
    --size_;
    return result;
}

void FdHandoffQueue::close() {
    const std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}

FdHandoffQueueStatus FdHandoffQueue::status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return {slots_.size(), size_, closed_};
}
} // namespace siftwing::net
