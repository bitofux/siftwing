/*
 * PROJECT : SIFTWING
 * FILE    : eventfd_wakeup.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 有限预算的非阻塞eventfd通知与清空原语
 */
#pragma once

#include "siftwing/net/unique_fd.hpp"
#include <cstddef>
#include <system_error>
#include <variant>

namespace siftwing::net {
/** @brief 成功独占eventfd（含fd 0），失败保存generic_category下的原errno。 */
using EventFdResult = std::variant<UniqueFd, std::error_code>;
/** @brief 每次通知最多尝试64次write；默认drain最多尝试64次read，含EINTR。 */
inline constexpr std::size_t eventfd_attempt_limit = 64;
/**
 * @brief 单次eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)，不使用EFD_SEMAPHORE
 *
 * @return
 *     独占拥有者或原errno；创建EINTR不重试，不退化成阻塞fd。
 *
 * @note
 *     目标消费者负责注册和最终关闭；本层不启动线程，不保持errno，无分配/异常。
 */
[[nodiscard]] EventFdResult create_eventfd_wakeup() noexcept;
/**
 * @brief 向稳定借用的非阻塞普通eventfd写入1，有限重试EINTR
 * @param[in] stable_fd
 *     create_eventfd_wakeup所建资源的稳定借用；须到所有发布+通知及最后控制通知返回后才关闭。
 *
 * @return
 *     成功或EAGAIN返回空error_code（已有合并通知）；预算耗尽返回EINTR，短写返回EIO，
 *     其他失败保留原errno；负fd预检EBADF。不关闭fd，不保持errno，无分配/异常。
 *
 * @note
 *     EAGAIN只在上述eventfd前提下表示已有可读通知；不能用于普通文件/管道。它不证明队列非空。
 */
[[nodiscard]] std::error_code notify_eventfd(int stable_fd) noexcept;
/** @brief drained是观察到EAGAIN；budget_exhausted不能证明已清空，failed含诊断。 */
enum class EventFdDrainState {
    drained,
    budget_exhausted,
    failed
};
/** @brief 只记录成功read次数；忽略内核计数值，它不是队列条目数。 */
struct EventFdDrainResult {
    EventFdDrainState state;      ///< 是否观察到暂时无通知、预算耗尽或错误。
    std::size_t successful_reads; ///< 本调用成功读取次数，不是通知数或工作数。
    std::error_code error;        ///< 仅failed非空；预算耗尽是显式状态。
};
/**
 * @brief 有限批量清通知；成功read与EINTR均占一次尝试
 *
 * @param[in] stable_fd
 *     同notify_eventfd的稳定非阻塞普通eventfd借用；调用方安排单消费者。
 *
 * @param[in] max_attempts
 *     正预算；0返回EINVAL，负fd优先EBADF。
 *
 * @return
 *     EAGAIN为drained；耗尽为budget_exhausted；短读/EOF为EIO，其他错误保存errno。
 *
 * @note
 *     消费者先清通知再从真实队列取有限批次；不得按通知数pop，也不得据drained推断队列为空。
 *     剩余队列非空时继续处理，不能直接无限等待；预算耗尽可在下一轮再清通知。无关闭/分配/异常。
 */
[[nodiscard]] EventFdDrainResult drain_eventfd(
    int stable_fd, std::size_t max_attempts = eventfd_attempt_limit) noexcept;
} // namespace siftwing::net
