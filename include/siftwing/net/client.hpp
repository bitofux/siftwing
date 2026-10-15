/*
 * PROJECT : SIFTWING
 * FILE    : client.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 单请求非阻塞TCP交换与总期限合同
 */
#pragma once
#include "siftwing/base/result.hpp"
#include "siftwing/net/frame_codec.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace siftwing::net {
/** @brief 一次交换的拥有配置；仅数字IP，不解析DNS，不修改全局信号。 */
struct ClientConfig {
    std::string host = "127.0.0.1";  ///< 数字IPv4或IPv6，不含端口/方括号/zone。
    std::uint16_t port = 0;          ///< 必须非零；端口与host分开。
    std::uint64_t timeout_ms = 5000; ///< [1,86400000]，连接/发送/接收共用总期限。
    std::size_t max_response_bytes = frame_max_body_bytes; ///< [2,1MiB]，按Body字节计。
};
/**
 * @brief 新建独占nonblocking/CLOEXEC连接，发送一帧并接收一个拥有响应
 *
 * @param[in] config
 *     调用期间稳定借用，函数不保存；非法配置返回client.config。
 * @param[in] request
 *     完整拥有请求的稳定借用；须通过server方向codec准入。
 *
 * @return
 *     已验证响应（含ERROR_RESPONSE），或拥有本地失败诊断；无部分成功。
 *
 * @throws
 *     分配及未知异常传播，局部RAII关闭socket；源请求保持。
 *
 * @note 无重试、DNS、TLS或跨调用连接；ERROR_RESPONSE不代表传输失败。
 *     固定steady_clock期限不因进展/EINTR刷新，不承诺调度硬实时；每次I/O前检查。
 *     核响应方向/类型/ID/JSON/预算，拒绝同次recv中首帧后的余量；不等待EOF确认未来余量。
 *     独立调用可并发，无共享可变状态；不会shutdown或接管调用者fd。
 */
[[nodiscard]] base::Result<Frame> exchange(const ClientConfig &config, const Frame &request);
} // namespace siftwing::net
