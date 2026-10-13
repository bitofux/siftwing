/*
 * PROJECT : SIFTWING
 * FILE    : epoll_ops.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : Linux LT epoll创建及借用fd注册的单次调用合同
 */
#pragma once

#include "siftwing/net/unique_fd.hpp"
#include <cstdint>
#include <sys/epoll.h>
#include <system_error>
#include <variant>

namespace siftwing::net {
/** @brief 成功分支独占epoll fd，错误分支保存generic_category下的errno。 */
using EpollResult = std::variant<UniqueFd, std::error_code>;
/** @brief 支持的LT关注位；0允许暂停普通读写通知，ERR/HUP仍由内核自动报告。 */
inline constexpr std::uint32_t epoll_interest_mask = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
/**
 * @brief 单次epoll_create1(EPOLL_CLOEXEC)，成功fd交给UniqueFd
 *
 * @return
 *     独占epoll fd或原errno，包括EINTR；不隐藏重试，不用fcntl退化。
 *
 * @note epoll拥有注册集合但不拥有目标fd；销毁epoll不会关闭目标fd。本层没有等待或事件分发。
 *     返回值自身无诊断文本分配，不抛异常，不承诺保持errno。
 */
[[nodiscard]] EpollResult create_epoll() noexcept;
/**
 * @brief 最多一次EPOLL_CTL_ADD，仅借用epoll与目标fd
 *
 * @param[in] epoll_fd
 *     调用期间稳定的epoll fd；负值返回EBADF。
 * @param[in] fd
 *     调用期间稳定的目标fd；负值返回EBADF。调用方负责注册期间的生命与串行使用。
 * @param[in] interests
 *     epoll_interest_mask的任意子集含0；其他位返回EINVAL且不调用内核，不支持ET/ONESHOT。
 * @param[in] token
 *     完整64位不透明值，含0；内核复制到event.data.u64，不是指针或fd拥有权。
 *
 * @return
 *     成功为空error_code；失败原errno。EEXIST明确返回，不自动改为MOD；失败不关闭目标fd。
 *
 * @note 所属I/O线程或外部同步下调用；本层无注册镜像/锁/回调。ERR/HUP无需订阅，本次成功不表示
 *     业务完成。关闭原fd时若仍有dup别名，旧注册可能存活；应先DEL再close。DEL也不清除已经
 *     取到的事件批次，上层仍须验证token对应对象及generation，不能仅按复用的fd整数派发。
 *     无分配/异常，不承诺errno保持，不自动重试EINTR或提供回滚操作。
 */
[[nodiscard]] std::error_code epoll_add(
    int epoll_fd, int fd, std::uint32_t interests, std::uint64_t token) noexcept;
/**
 * @brief 最多一次EPOLL_CTL_MOD，替换全部关注位和token
 *
 * @param[in] epoll_fd
 *     稳定借用的epoll fd，线程与所有权边界同epoll_add。
 * @param[in] fd
 *     已注册且保持打开的原目标fd。
 * @param[in] interests
 *     同epoll_add的LT掩码；不是按位增减，调用方给出完整新值。
 * @param[in] token
 *     完整替换event.data.u64的值；已取出的旧事件不会被更新。
 *
 * @return
 *     成功为空；失败原errno，旧注册保持。ENOENT不自动改为ADD，不关闭目标或epoll。
 *
 * @note 预检、单次调用、无异常/分配及errno边界同epoll_add。
 */
[[nodiscard]] std::error_code epoll_modify(
    int epoll_fd, int fd, std::uint32_t interests, std::uint64_t token) noexcept;
/**
 * @brief 最多一次EPOLL_CTL_DEL，event参数为nullptr，不关闭任意fd
 *
 * @param[in] epoll_fd
 *     稳定借用的epoll fd，线程边界同epoll_add。
 * @param[in] fd
 *     保持打开的原注册目标fd；不能用复用的整数替代原fd身份。
 *
 * @return
 *     成功为空；负fd返回EBADF，系统失败保存原errno。重复注销的ENOENT明确返回，
 *     不冒称清理成功、不重试EINTR。失败由上层决定诊断/重试/终止，不修改其拥有权。
 *
 * @note Linux关闭最后一个指向目标open file description的fd也会自动移除注册；有dup别名
 *     时不能依赖该清理。DEL后已取出的事件仍须上层失效检查，无分配/异常，不承诺errno保持。
 */
[[nodiscard]] std::error_code epoll_remove(int epoll_fd, int fd) noexcept;
} // namespace siftwing::net
