/*
 * PROJECT : SIFTWING
 * FILE    : protocol_connection_config.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-21
 * BRIEF   : 与监听及线程拓扑无关的固定连接处理配置
 */
#pragma once
#include "siftwing/net/connection.hpp"
#include "siftwing/net/completion_wakeup.hpp"
#include "siftwing/net/service_info_source.hpp"

namespace siftwing::logging {
class Logger;
}
namespace siftwing::net {
/**
 * @brief 所属I/O线程的连接预算与稳定协作借用；不创建线程或fd移交队列
 * @note OnlineServer将总连接/结果默认设置为256；旧独立Sub仍默认128且限制至1024。
 *     全部字段启动前冻结，元信息借用覆盖Core全部调用；容量不是RSS/CPU硬限。
 */
struct ProtocolConnectionConfig {
    std::size_t max_connections = 128; ///< [1,65536]固定连接槽；满时关闭新源。
    std::size_t max_body_bytes = frame_max_body_bytes; ///< [2,frame_max_body_bytes]每帧Body上限。
    std::size_t read_byte_budget = 65536; ///< [1,24+frame_max_body_bytes]每连接每轮recv字节。
    std::size_t write_byte_budget = 65536; ///< 同范围，每连接每轮send字节。
    std::size_t io_attempt_budget = 16;    ///< [1,1024]read/flush各自尝试预算。
    std::uint32_t reactor_id = 0; ///< 固定身份命名空间；旧结果存活时不能重建并复用。
    std::size_t completion_capacity = 128; ///< [1,65536]拥有结果槽，陈旧结果也占容量。
    std::size_t completion_budget = 16; ///< [1,1024]每轮最多应用结果数，与通知计数无关。
    RoutedRequestSubmitter *request_submitter = nullptr; ///< 可选稳定借用，不拥有worker。
    OutputWatermarks output;          ///< 全0按Body解析；非法组合在启动前拒绝。
    std::int64_t idle_timeout_ms = 0; ///< [0,86400000]；0关闭，接纳/排空起，recv正字节刷新。
    std::int64_t request_timeout_ms = 0; ///< 同范围；交付至输出排空，进展不延长。
    logging::Logger *logger = nullptr; ///< 稳定借用覆盖最后Core/worker调用，日志可能阻塞。
    std::optional<std::uint64_t> log_index_version; ///< 固定元信息；动态service_info优先。
    std::string server_info_body; ///< 可选拥有JSON；启动后固定，动态来源优先。
    const ServiceInfoSource *service_info = nullptr; ///< 可选只读借用，覆盖Core完整生命周期。
};
} // namespace siftwing::net
