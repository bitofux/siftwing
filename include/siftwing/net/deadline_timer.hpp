/*
 * PROJECT : SIFTWING
 * FILE    : deadline_timer.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : CLOCK_MONOTONIC绝对单次timerfd与有限读取
 */
#pragma once
#include "siftwing/net/deadline_queue.hpp"
#include "siftwing/net/unique_fd.hpp"
#include <system_error>
#include <variant>

namespace siftwing::net {
/** @brief 同钟非负纳秒或generic_category下errno；检查int64换算上界，越界为EOVERFLOW。 */
[[nodiscard]] std::variant<DeadlineTime, std::error_code> monotonic_now() noexcept;
/**
 * @brief 创建CLOCK_MONOTONIC、TFD_NONBLOCK|TFD_CLOEXEC、初始disarmed的独占timerfd
 * @return
 *     UniqueFd（0也合法）或原errno；单次create不重试，不分配/抛异常，不保持errno。
 * @note 所属Sub创建/注册/操作/注销/关闭；调用者不得向worker发布fd或同时关闭借用。
 */
[[nodiscard]] std::variant<UniqueFd, std::error_code> create_deadline_timer() noexcept;
/**
 * @brief 以TFD_TIMER_ABSTIME设一次期限；nullopt显式disarm，无周期
 * @param[in] stable_fd
 *     上述timerfd的稳定独占线程借用；负值优先返回EBADF。
 * @param[in] when
 *     同monotonic_now的正绝对纳秒；<=0返回EINVAL而非误解为disarm。过去期限可立即可读。
 * @return
 *     成功为空，失败原errno；单次settime不重试（含EINTR），失败不得假设已设时。
 * @note 不读取队列；调用者以queue.next().when设最小值，变化后重新设时并处理诊断。
 *     重设/取消可清内核旧到期计数；已经取得的epoll批次可能仍含陈旧就绪，EAGAIN正常。
 *     无分配/异常，不保持errno；不是硬实时触发或任务取消。
 */
[[nodiscard]] std::error_code arm_deadline_timer(
    int stable_fd, std::optional<DeadlineTime> when) noexcept;
/** @brief drained观察到EAGAIN；预算耗尽不证明已清空；failed保留诊断。 */
enum class TimerDrainState { drained, budget_exhausted, failed };
/** @brief 内核计数值不作deadline数，成功读取次数只用于就绪观测。 */
struct TimerDrainResult {
    TimerDrainState state; ///< drained/budget_exhausted/failed。
    std::size_t successful_reads; ///< 本次成功read次数，不等于到期条目数。
    std::error_code error; ///< 仅failed非空。
};
/**
 * @brief 有限读取普通单次timerfd，EINTR与成功read均占预算
 * @param[in] stable_fd
 *     同上稳定非阻塞借用，调用方串行安排唯一消费者。
 * @param[in] max_attempts
 *     正预算，默认64；0返回EINVAL，负fd优先EBADF。
 * @return
 *     EAGAIN为drained，短读/EOF为EIO，其他错误原errno；预算耗尽显式返回。
 * @note 清就绪后仍须重新读同钟时间、按容器取有限到期批次并重设最小期限；不按计数pop。
 *     此函数不操作容器，不关闭/分配/抛异常，不保持errno。
 */
[[nodiscard]] TimerDrainResult drain_deadline_timer(
    int stable_fd, std::size_t max_attempts = 64) noexcept;
} // namespace siftwing::net
