/*
 * PROJECT : SIFTWING
 * FILE    : fd_handoff_queue.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 固定容量FIFO的fd独占所有权提交与关闭入口
 */
#pragma once

#include "siftwing/net/unique_fd.hpp"
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace siftwing::net {
/** @brief 入队结果；只有accepted消费源，其余分支保持源拥有者。 */
enum class FdHandoffResult {
    accepted,   ///< 已提交到队列，源为空；后续通知错误不撤销提交。
    closed,     ///< 发布入口已关闭，优先于空源和满队列检查。
    invalid_fd, ///< 源为空，入口开放时优先于满队列检查。
    full        ///< 有效源但容量已满；不等待空位。
};
/** @brief 同一锁内复制的观测值，不保证下一时刻仍有容量或条目。 */
struct FdHandoffQueueStatus {
    std::size_t capacity; ///< 构造后不变的正容量。
    std::size_t size;     ///< 仍由队列拥有的条目数，不大于capacity。
    bool closed;          ///< 单调关闭标志；关闭后仍允许取出已有条目。
};
/**
 * @brief 预分配有锁有界fd队列；多生产者可发布，调用方安排单目标消费者
 *
 * @note 所有操作使用同一mutex，提供发布可见性与FIFO提交顺序；不等待容量但可等待锁。
 *     正常返回路径不分配；标准库构造加锁异常时可能分配，不在本层无分配承诺内。
 *     每个源须由其生产者独占，禁止其他线程同时操作该源。组件不绑定线程、不通知、不注册fd。
 *     所有调用要求对象存活；正常停止须关闭入口、撤销发布者并等其返回，再由目标线程
 *     drain并在锁外关闭，join后销毁队列。析构时不得有活跃调用者。
 */
class FdHandoffQueue final {
public:
    /**
     * @brief 创建空且开放的固定容量队列；一次性预分配全部槽位
     * @param[in] capacity
     *     正的条目上限，不表示字节或通知计数。
     *
     * @throws
     *     0抛invalid_argument；超过vector可表示范围抛length_error；分配失败抛bad_alloc。
     */
    explicit FdHandoffQueue(std::size_t capacity);
    /** @brief RAII兜底在析构线程关闭残留条目、忽略诊断；不能替代目标线程drain协议。 */
    ~FdHandoffQueue() noexcept = default;
    FdHandoffQueue(const FdHandoffQueue &) = delete;
    FdHandoffQueue &operator=(const FdHandoffQueue &) = delete;
    FdHandoffQueue(FdHandoffQueue &&) = delete;
    FdHandoffQueue &operator=(FdHandoffQueue &&) = delete;

    /**
     * @brief 尝试提交一个独占fd，不等待容量；正常返回路径不分配，不调用close
     * @param[in,out] source
     *     accepted后为空；其他结果及加锁失败时保持原拥有者，不接受临时对象。
     *
     * @return
     *     同一锁内按closed、invalid_fd、full、accepted的顺序确定结果；成功move是提交点。
     *
     * @throws
     *     mutex加锁失败可抛system_error，发生在提交前，队列和源保持；不承诺保持errno。
     */
    [[nodiscard]] FdHandoffResult try_push(UniqueFd &source);
    /**
     * @brief 取出最早提交的拥有者；关闭入口不阻止消费
     * @return
     *     空队列返回nullopt，否则按值转交UniqueFd，调用方负责后续注册或在锁外关闭。
     *
     * @throws
     *     mutex加锁失败可抛system_error，队列保持。正常返回路径不分配、不close、不等条目。
     */
    [[nodiscard]] std::optional<UniqueFd> try_pop();
    /** @brief 同一锁内幂等关闭发布入口，不清空条目；加锁失败可抛system_error且状态保持。 */
    void close();
    /** @brief 加锁复制状态；正常返回不分配、不暴露内部拥有者，加锁失败可抛system_error。 */
    [[nodiscard]] FdHandoffQueueStatus status() const;

private:
    std::vector<UniqueFd> slots_; ///< 固定长度；活跃环区外的槽位必须为空，析构兜底拥有残留fd。
    mutable std::mutex mutex_; ///< 保护槽位、head_、size_与closed_，锁内不做资源清理或回调。
    std::size_t head_ = 0; ///< 下一条目的位置，始终小于slots_.size()。
    std::size_t size_ = 0; ///< 当前条目数；由push/pop在锁内更新。
    bool closed_ = false;  ///< 只由close在锁内置true，不可重开。
};
} // namespace siftwing::net
