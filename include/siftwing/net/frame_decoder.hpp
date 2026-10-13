/*
 * PROJECT : SIFTWING
 * FILE    : frame_decoder.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 从有界 Buffer 逐帧取得拥有型载荷，保留未完成帧
 */

#pragma once

#include "siftwing/net/buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace siftwing::net {

/** @brief 版本 1 固定帧头字节数，不依赖 C++ 对象布局。 */
inline constexpr std::size_t frame_header_bytes = 24;
/** @brief 版本 1 Body 硬上限，按序列化后的字节计量。 */
inline constexpr std::size_t frame_max_body_bytes = 1048576;

/** @brief 版本 1 唯一消息编号；未知值不能按高位推导为合法消息。 */
enum class MessageType : std::uint16_t {
    ping = 0x0001,                 ///< 客户端心跳请求。
    server_info_request = 0x0002,  ///< 客户端服务信息请求。
    search_request = 0x0003,       ///< 客户端搜索请求。
    suggest_request = 0x0004,      ///< 客户端推荐请求。
    pong = 0x8001,                 ///< 服务端心跳响应。
    server_info_response = 0x8002, ///< 服务端服务信息响应。
    search_response = 0x8003,      ///< 服务端搜索响应。
    suggest_response = 0x8004,     ///< 服务端推荐响应。
    error_response = 0xffff        ///< 服务端错误响应，status 为 1—6。
};

/** @brief 接收端角色，server 只接受请求，client 只接受响应。 */
enum class ReceiveRole {
    server, ///< 服务端接收，仅请求方向合法。
    client  ///< 客户端接收，仅响应方向合法。
};

/**
 * @brief 头部已验证且字节完整的拥有型帧，不代表 JSON 或业务验证成功
 *
 * @note 值可复制/移动；Body 与原 Buffer 无借用关系，可在 Buffer 修改或销毁后使用。
 *     解码不检查 UTF-8、JSON、PING/PONG 的 {} 或业务字段，调用者仍须验证 Body。
 */
struct Frame final {
    MessageType message_type; ///< 已验证的线编号，解码时写入，随后由调用者拥有。
    std::uint16_t status; ///< 正常消息为 0；error_response 为 1—6，不是解码错误码。
    std::uint64_t request_id; ///< 非零连接内关联号，不替代连接 generation。
    std::string body;         ///< 独占原始 Body 字节，无隐含 NUL 或字符计量。
};

/** @brief 头部合法性/存储拒绝原因；不是线上 ERROR_RESPONSE 状态。 */
enum class DecodeError {
    magic,        ///< magic 不为 SFTW。
    version,      ///< version 不为 1。
    header_size,  ///< header_size 不为 24。
    message_type, ///< 未知消息编号。
    flags,        ///< flags 非零。
    status,       ///< 类型与 status 组合非法。
    body_length,  ///< 长度不在 [2,1048576] 或超过本地 Body 上限。
    request_id,   ///< request_id 为零。
    direction,    ///< 消息与接收端角色不符。
    buffer_limit  ///< 帧总长无法由给定 Buffer 的上限容纳。
};

/** @brief 三分支结果：monostate 需要更多字节，Frame 完整成功，DecodeError 拒绝。 */
using DecodeResult = std::variant<std::monostate, Frame, DecodeError>;

/**
 * @brief 按既定顺序验证首帧头，完整时消费恰好一帧并返回拥有型 Body
 *
 * @param[in,out] input
 *     由调用者拥有和串行使用的字节缓冲区；调用期间不能并发修改。
 * @param[in] role
 *     本地接收端角色，必须为 server 或 client。
 * @param[in] max_body_bytes
 *     本地 Body 上限，必须在 [2,frame_max_body_bytes]，默认协议硬上限。
 *
 * @return
 *     monostate：帧头或 Body 不完整，input 完全保持；Frame：input 消费首帧，
 *     后续字节保持顺序；DecodeError：input 完全保持，调用者须终止该字节流，
 *     不扫描 magic 重同步、不据非法头生成错误响应。
 *
 * @throws std::invalid_argument
 *     配置或角色非法，input 保持。
 * @throws
 *     Body 分配异常传播，input 的字节、索引、容量与原 view 均保持，可重试。
 *
 * @note 不保存半帧状态、无回调或系统 I/O。循环取帧必须在 monostate/错误时停止。
 *     EOF 后非空且不完整的 input 由上层判为截断；本函数不知道 EOF。
 *     结果构造后才消费，返回移动不抛异常；成功消费后旧 Buffer view 须重新获取。
 */
[[nodiscard]] DecodeResult decode_next(
    Buffer &input, ReceiveRole role, std::size_t max_body_bytes = frame_max_body_bytes);

} // namespace siftwing::net
