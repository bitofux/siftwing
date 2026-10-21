/*
 * PROJECT : SIFTWING
 * FILE    : reactor_report.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-21
 * BRIEF   : 单生产Reactor的生命周期与无资源借用连接统计
 */
#pragma once
#include "siftwing/metrics/server_metrics.hpp"
#include <cstddef>
#include <system_error>

namespace siftwing::net {
/** @brief draining只推进已在途；quiescing停止业务等发布者；stopped已完成全部发布者屏障及网络清理。 */
enum class ReactorState {
    idle,
    running,
    quiescing,
    stopped,
    draining
};
/** @brief 所属调用线程的按值观测，不授予连接借用，也不授予跨线程读取可变状态的权限。 */
struct ReactorReport {
    ReactorState state = ReactorState::idle; ///< 生命周期，禁止从quiescing推断可销毁。
    bool stop_requested =
        false; ///< 立即停止请求，不等于线程退出或响应完成；排空另见drain_started。
    std::size_t received = 0; ///< Listener直接交付的准入尝试数，含注册前拒绝。
    std::size_t admitted = 0; ///< 曾成功构造并ADD的连接数，归属本Reactor直到终止。
    std::size_t rejected = 0;  ///< received中未注册的源，由所属Reactor关闭。
    std::size_t dropped = 0;   ///< 曾注册后释放的连接数，含停止时丢弃。
    std::size_t active = 0;    ///< 最近发布的活跃连接数，最终为0。
    std::size_t requests = 0;  ///< 完整受信请求数，含Body拒绝或未接业务的类型。
    std::size_t responses = 0; ///< 输出排空且finish成功，不证明对端收到。
    std::error_code first_error;            ///< 启动/运行首因，正常停止为空。
    std::error_code cleanup_error;          ///< 首个独立清理诊断，不覆盖运行首因。
    std::error_code first_connection_error; ///< 局部客户端错误，不自动终止Reactor。
    std::size_t completion_received = 0; ///< 正常调度取出结果数，等于applied+stale+rejected。
    std::size_t completion_applied = 0; ///< 已复制结果或硬限替代ERROR，不证明发送完成。
    std::size_t completion_stale = 0; ///< 身份表拒绝的旧代次/未知slot结果，锁外销毁。
    std::size_t completion_rejected = 0; ///< 当前身份但无等待请求/不匹配/应用异常的结果。
    std::size_t completion_discarded = 0; ///< 停止且发布者静止后直接销毁的残留，不应用。
    std::size_t output_limit_rejections = 0; ///< 合法关联响应被输出硬限拒绝次数，含直接错误响应。
    std::size_t output_limit_closes =
        0; ///< 硬限拒绝后固定RESOURCE_LIMIT也无法入队/构造的局部关闭。
    std::size_t idle_timeouts = 0; ///< 空闲期限到期而关闭的连接数；不含停止清理。
    std::size_t request_timeouts = 0; ///< 请求期限到期而关闭的连接数，包含输出未排空。
    metrics::ConnectionIo io; ///< 已关闭与仍活跃连接累计I/O之和；高水位取所有曾存活连接的最大值。
    std::uint64_t input_bytes = 0, output_bytes = 0; ///< 最近发布的活跃连接用户缓冲总可读字节。
    std::uint64_t protocol_errors = 0; ///< 坏头/截帧各一次，完整坏Body逐请求一次，业务错误不计。
    std::array<std::uint64_t, 4> request_types{}; ///< PING/INFO/SEARCH/SUGGEST交付数，包括坏Body。
    std::array<std::uint64_t, 7> response_statuses{}; ///< 实际成功入队的status 0..6，非网络排空。
    bool drain_started = false; ///< 所属线程已观察排空请求，此后不读取新请求。
    std::size_t drain_responses = 0; ///< 排空阶段finish成功后关闭的连接数，不证明对端收到。
    std::size_t drain_timeouts = 0; ///< 排空截止时仍pending而关闭的连接数，非服务运行错误。
};
} // namespace siftwing::net
