/*
 * PROJECT : SIFTWING
 * FILE    : protocol_connection_core.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-20
 * BRIEF   : 单一所属I/O线程逐轮驱动的生产连接处理内部边界
 */
#pragma once

#include "siftwing/net/protocol_sub_reactor.hpp"
#include <memory>

namespace siftwing::net {
/**
 * @brief 拥有连接槽、身份表与期限队列，借用所属I/O线程的轮询资源和固定配置
 * @note
 *     仅创建它的I/O线程可调用除构造/析构外的成员；本类型不创建线程、不监听、不消费外层队列。
 *     epoll、timer、连接config、report和drain_requested均为稳定借用，覆盖本对象完整生命周期。
 *     close先撤身份再移除epoll并关闭连接；析构前调用者仍负责停止发布者及清空外层队列。
 */
class ProtocolConnectionCore final {
public:
    ProtocolConnectionCore(int epoll,
        int timer,
        const ProtocolConnectionConfig &config,
        ProtocolSubStatus &report,
        const std::atomic<std::int64_t> &drain_requested);
    ~ProtocolConnectionCore() noexcept;
    ProtocolConnectionCore(const ProtocolConnectionCore &) = delete;
    ProtocolConnectionCore &operator=(const ProtocolConnectionCore &) = delete;
    ProtocolConnectionCore(ProtocolConnectionCore &&) = delete;
    ProtocolConnectionCore &operator=(ProtocolConnectionCore &&) = delete;

    /** @brief 观察排空后判断连接核心是否已无活跃连接。 */
    [[nodiscard]] bool drained();
    /** @brief 接管一个已接受fd，分配身份并注册到借用的epoll。 */
    void admit(UniqueFd &source);
    /** @brief 推进可运行连接、期限及一轮epoll等待；timeout语义沿用原Sub。 */
    void tick(int timeout);
    /** @brief 新一轮：重置逐连接预算并推进主动续处理/期限；返回本轮是否已有主动工作。 */
    [[nodiscard]] bool prepare_round();
    /** @brief wait后先按同钟裁决到期，再应用本批事件/Completion；不重置逐连接预算。 */
    void check_deadlines();
    /** @brief 仅分派非零连接token；未知/陈旧忽略，控制来源由唯一外层循环处理。 */
    void dispatch(std::uint64_t token, std::uint32_t events);
    /** @brief 校验身份和请求状态后应用一个拥有型Completion。 */
    void complete(const concurrency::RoutedCompletion &result);
    /** @brief 将已关闭累计与当前活跃连接指标复制到借用报告。 */
    void observe_metrics() noexcept;
    /** @brief 由所属线程撤销身份、移除epoll并关闭全部剩余连接。 */
    void close() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_; ///< 拥有全部连接状态；借用对象只保存在Impl内。
};
} // namespace siftwing::net
