/*
 * PROJECT : SIFTWING
 * FILE    : signalfd_source.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 终止信号的线程屏蔽、独占fd与有限读取合同
 */
#pragma once

#include "siftwing/net/unique_fd.hpp"
#include <cstddef>
#include <system_error>
#include <variant>

namespace siftwing::net {
/** @brief 独占信号fd（0合法），或generic_category下的系统错误。 */
using SignalFdResult = std::variant<UniqueFd, std::error_code>;
/** @brief 默认最多64次read，EINTR也占一次尝试。 */
inline constexpr std::size_t signalfd_attempt_limit = 64;
/** @brief 暂时读空、有限预算耗尽或失败；读空不证明未来没有信号。 */
enum class SignalFdDrainState {
    drained,
    budget_exhausted,
    failed
};
/** @brief 本次读取的拥有值，不携带fd或记录存储借用。 */
struct SignalFdDrainResult {
    SignalFdDrainState state; ///< 本次停止原因；预算耗尽后由LT再次调度。
    int signal_number; ///< 本次首条SIGINT/SIGTERM，0表示尚无；失败也保留已读事实。
    std::size_t records; ///< 完整受管记录数；标准信号可合并，不是发送次数。
    std::error_code error; ///< failed时原errno；短读/EOF/非法记录为EIO。
};
/**
 * @brief 调用线程以SIG_BLOCK加入SIGINT/SIGTERM，不改变其他mask位或disposition
 * @return
 *     成功为空；pthread_sigmask的直接返回码映射generic_category，不从errno推断。
 * @note 不创建线程、handler或恢复器，无分配/异常。进程入口须在任何线程创建前调用，
 *     保证所有可能接收进程信号的线程继承并保持屏蔽。关闭fd不清除pending信号；
 *     恢复mask可能触发默认终止，恢复策略完全属于调用者。
 */
[[nodiscard]] std::error_code block_termination_signals() noexcept;
/**
 * @brief 预检当前线程已屏蔽两受管信号，创建NONBLOCK|CLOEXEC signalfd
 * @return
 *     完整独占fd或原系统错误；未屏蔽返回EINVAL；失败不发布部分资源。
 * @pre
 *     调用者保证其他相关线程也已屏蔽；本函数仅检查当前线程，不能验证进程全部线程。
 * @note 不改mask/disposition，不创建线程；无分配/异常。拥有者在所属线程注册、读取、
 *     注销后关闭，不允许外部关闭/复用。线程定向给其他线程的信号不保证在本fd读取。
 */
[[nodiscard]] SignalFdResult create_termination_signalfd() noexcept;
/**
 * @brief 有限读取完整signalfd_siginfo，只接受SIGINT/SIGTERM
 * @param[in] stable_fd
 *     稳定借用非阻塞受管signalfd，调用期间不得关闭/复用；负值返回EBADF。
 * @param[in] max_attempts
 *     正数尝试上限，0返回EINVAL；含EINTR，持续信号不会无限占用线程。
 * @return
 *     EAGAIN为drained；其他读错误为failed；预算耗尽显式报告并保留首信号/记录数。
 * @note 无分配/异常，不关闭资源或处理业务；同一fd读取须串行，不能将记录数作发送次数。
 */
[[nodiscard]] SignalFdDrainResult drain_termination_signalfd(
    int stable_fd, std::size_t max_attempts = signalfd_attempt_limit) noexcept;
} // namespace siftwing::net
