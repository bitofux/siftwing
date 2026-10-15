/*
 * PROJECT : SIFTWING
 * FILE    : deadline_queue.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 不积累陈旧条目的固定容量绝对期限容器
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace siftwing::net {
/** @brief CLOCK_MONOTONIC自其纪元起的纳秒；有效期限为[1,INT64_MAX]，不是墙钟或相对时长。 */
using DeadlineTime = std::int64_t;
/** @brief 纯值条目；不拥有连接/回调，key不自动证明身份存活或请求仍有效。 */
struct DeadlineEntry {
    std::uint64_t key = 0; ///< 调用方分配的不透明非零key；生命周期/复用由调用方负责。
    DeadlineTime when = 0; ///< 正的CLOCK_MONOTONIC绝对纳秒，等于当前时间即到期。
};
/** @brief stored为插入或更新；full与invalid均保持整个容器不变。 */
enum class DeadlineUpdate { stored, full, invalid };
/**
 * @brief 所属线程串行操作的有界期限集合，无fd/锁/时钟读取/回调，不复制或移动
 * @note 预分配后操作不分配、不抛异常；查找/更新/最小值/取消/取到期均O(capacity)。
 *     同key只占一个条目，更新与取消不留下陈旧项；相同时间按较小key先取。
 *     容器变化与timerfd设时是两个阶段；设时失败不撤销已完成的容器变化，调用者必须处理。
 */
class DeadlineQueue final {
public:
    /** @brief 容量限[1,2048]；非法抛invalid_argument，预分配可抛bad_alloc/length_error。 */
    explicit DeadlineQueue(std::size_t capacity);
    ~DeadlineQueue() noexcept = default;
    DeadlineQueue(const DeadlineQueue &) = delete;
    DeadlineQueue &operator=(const DeadlineQueue &) = delete;
    DeadlineQueue(DeadlineQueue &&) = delete;
    DeadlineQueue &operator=(DeadlineQueue &&) = delete;
    /**
     * @brief 同key原位替换，否则有空位才插入；满容量仍允许更新
     * @param[in] entry
     *     key非零且when为正；先校验，再找已有key，最后判满。允许过去的绝对期限。
     * @return
     *     stored/full/invalid，无源拥有权交接；不读取当前时间。
     */
    [[nodiscard]] DeadlineUpdate upsert(DeadlineEntry entry) noexcept;
    /** @brief 精确key删除，存在返回true；未知/0返回false且保持。 */
    [[nodiscard]] bool erase(std::uint64_t key) noexcept;
    /** @brief 返回最小(when,key)的拥有值，空为nullopt；值不证明连接/请求有效。 */
    [[nodiscard]] std::optional<DeadlineEntry> next() const noexcept;
    /** @brief 最小when<=now时删除并返回它，否则保持；now<=0无到期。调用方提供同钟纳秒。 */
    [[nodiscard]] std::optional<DeadlineEntry> pop_due(DeadlineTime now) noexcept;
    /** @brief 当前真实条目数，不含任何取消/更新的旧副本。 */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    /** @brief 构造冻结的条目上限，不表示fd、RSS或全局请求预算。 */
    [[nodiscard]] std::size_t capacity() const noexcept { return entries_.size(); }
private:
    std::vector<DeadlineEntry> entries_; ///< 构造时固定长度，只有[0,size_)为活跃条目。
    std::size_t size_ = 0; ///< 插入递增，真实取消/取到期递减，不扩容。
};
} // namespace siftwing::net
