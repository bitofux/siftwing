/*
 * PROJECT : SIFTWING
 * FILE    : deadline_queue.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 固定存储线性扫描与真实删除，不使用惰性失效堆
 */
#include "siftwing/net/deadline_queue.hpp"
#include <stdexcept>

namespace siftwing::net {
namespace {
std::size_t checked_capacity(std::size_t capacity) {
    if (capacity == 0 || capacity > 2048)
        throw std::invalid_argument("invalid DeadlineQueue capacity");
    return capacity;
}
bool earlier(DeadlineEntry a, DeadlineEntry b) noexcept {
    return a.when < b.when || (a.when == b.when && a.key < b.key);
}
} // namespace
DeadlineQueue::DeadlineQueue(std::size_t capacity) : entries_(checked_capacity(capacity)) {}
DeadlineUpdate DeadlineQueue::upsert(DeadlineEntry entry) noexcept {
    if (entry.key == 0 || entry.when <= 0)
        return DeadlineUpdate::invalid;
    for (std::size_t i = 0; i < size_; ++i)
        if (entries_[i].key == entry.key) {
            entries_[i] = entry;
            return DeadlineUpdate::stored;
        }
    if (size_ == entries_.size())
        return DeadlineUpdate::full;
    entries_[size_++] = entry;
    return DeadlineUpdate::stored;
}
bool DeadlineQueue::erase(std::uint64_t key) noexcept {
    for (std::size_t i = 0; i < size_; ++i)
        if (entries_[i].key == key) {
            entries_[i] = entries_[--size_]; // 无顺序依赖；最小值总由当前活跃集合重新计算。
            return true;
        }
    return false;
}
std::optional<DeadlineEntry> DeadlineQueue::next() const noexcept {
    if (size_ == 0)
        return std::nullopt;
    auto result = entries_[0];
    for (std::size_t i = 1; i < size_; ++i)
        if (earlier(entries_[i], result))
            result = entries_[i];
    return result;
}
std::optional<DeadlineEntry> DeadlineQueue::pop_due(DeadlineTime now) noexcept {
    const auto result = next();
    if (!result || result->when > now)
        return std::nullopt;
    (void)erase(result->key);
    return result;
}
} // namespace siftwing::net
