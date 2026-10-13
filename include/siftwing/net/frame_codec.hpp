/*
 * PROJECT : SIFTWING
 * FILE    : frame_codec.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 内存帧编码、独立 Body 准入与统一响应构造合同
 */

#pragma once

#include "siftwing/net/frame_decoder.hpp"

#include <optional>
#include <string_view>

namespace siftwing::net {

/** @brief 本地 JSON 容器嵌套上限；根对象计一层，不改变线上帧头。 */
inline constexpr std::size_t frame_max_json_depth = 64;

/** @brief 线上 ERROR_RESPONSE 状态；本地 ProtocolError 不直接转成这些编号。 */
enum class ErrorStatus : std::uint16_t {
    invalid_request = 1,   ///< 完整合法请求头下的非法 Body/业务参数。
    overloaded = 2,        ///< 任务未被容量接纳。
    stopping = 3,          ///< 停止接纳任务。
    internal_error = 4,    ///< 已接纳任务内部失败。
    deadline_exceeded = 5, ///< 处理超时，不保证取消任务。
    resource_limit = 6     ///< 业务或输出预算拒绝。
};

/** @brief 内存准入/构造错误，不表示网络关闭或错误响应已经发送。 */
enum class ProtocolError {
    message_type, ///< Frame 中未知类型。
    status,       ///< 类型与状态组合非法，或 error helper 状态未知。
    body_length,  ///< Body 字节长度不在 [2,max_body_bytes]。
    request_id,   ///< 关联号为零。
    direction,    ///< 类型与接收方角色不符。
    utf8,         ///< Body 不是严格 UTF-8。
    json,         ///< JSON 语法、转义或库数值表示失败；BOM 也拒绝。
    json_object,  ///< 完整 JSON 根值不是对象。
    json_depth,   ///< 容器嵌套超过本地 64 层，解析提前停止。
    heartbeat     ///< PING/PONG Body 不恰为两个字节 {}。
};

/** @brief 成功拥有完整线字节，失败只有本地错误，无部分输出。 */
using EncodeResult = std::variant<std::string, ProtocolError>;
/** @brief 成功拥有响应 Frame，失败只有本地错误。 */
using ResponseResult = std::variant<Frame, ProtocolError>;

/**
 * @brief 独立检查 Frame 字段及 Body，不改变 decode_next 的原始字节合同
 *
 * @param[in] frame
 *     调用期间稳定的借用值；检查类型、状态、长度、ID、方向后检查 Body。
 * @param[in] receiver
 *     对端接收角色，必须为 server 或 client。
 * @param[in] max_body_bytes
 *     本地准入上限 [2,frame_max_body_bytes]，等于上限合法。
 *
 * @return
 *     nullopt 为通过；否则为首个本地拒绝原因。Body 按 UTF-8→严格 JSON 对象→
 *     心跳检查，解析中遇深度预算立即拒绝，无 DOM，不验证业务字段/重复键。
 *
 * @throws std::invalid_argument
 *     非法角色或预算；其他分配异常上传，frame 保持。
 *
 * @note JSON 库不能表示的数值（如 1e999）拒绝；不重写合法 Body 的空白/键顺序。
 *     头/方向/长度错误关闭流，不据此猜 ID。合法请求头的 Body 错误可由上层
 *     构造错误响应；json_depth 是局部资源拒绝。无 EOF/socket/并发同步或发送动作。
 */
[[nodiscard]] std::optional<ProtocolError> validate_frame(
    const Frame &frame, ReceiveRole receiver, std::size_t max_body_bytes = frame_max_body_bytes);

/**
 * @brief 完整验证后逐字节生成固定版本 1 大端头和原样 Body
 *
 * @param[in] frame
 *     稳定同步借用，成功输出独立拥有，不引用输入。
 * @param[in] receiver
 *     将读取这些线字节的对端角色。
 * @param[in] max_body_bytes
 *     Body 字节准入上限，同 validate_frame。
 *
 * @return
 *     完整字符串或 ProtocolError，无部分结果；配置和异常行为同 validate_frame。
 *
 * @note 不追加 NUL，不写 Buffer/fd；失败/分配异常均不修改 frame。
 */
[[nodiscard]] EncodeResult encode_frame(
    const Frame &frame, ReceiveRole receiver, std::size_t max_body_bytes = frame_max_body_bytes);

/**
 * @brief 验证完整请求后映射对应成功响应并原值回传 request_id
 *
 * @param[in] request
 *     稳定借用的请求，头和 Body 都须通过服务端准入。
 * @param[in] body
 *     稳定借用的响应 Body；返回独立副本，须通过客户端准入，PONG 固定 {}。
 * @param[in] max_body_bytes
 *     同时适用于请求和响应的本地上限。
 *
 * @return
 *     status=0 的拥有响应或首个错误；配置/分配异常上传，输入保持。
 *
 * @note 不判断搜索参数、在途请求数或业务执行是否成功。
 */
[[nodiscard]] ResponseResult make_response(
    const Frame &request, std::string_view body, std::size_t max_body_bytes = frame_max_body_bytes);

/**
 * @brief 对合法完整请求头构造固定诊断 JSON 错误响应，允许请求 Body 非法
 *
 * @param[in] request
 *     稳定借用；重新检查类型/status/实际 Body 长度/ID/请求方向，跳过 Body 内容。
 * @param[in] status
 *     必须是六个 ErrorStatus 之一。
 * @param[in] max_body_bytes
 *     请求与错误 Body 均须容纳的上限；过小则返回 body_length，不递归生成错误。
 *
 * @return
 *     ERROR_RESPONSE、指定状态、原 ID、拥有固定 {"message":"..."}；或本地错误。
 *     配置/分配异常上传，输入保持。
 *
 * @warning 只能用于 decode_next 完整交付或同等受信构造的请求，不能从损坏头猜 ID。
 *     message 仅诊断，不公开任意异常文本；客户端按头 status 判类。
 */
[[nodiscard]] ResponseResult make_error_response(
    const Frame &request, ErrorStatus status, std::size_t max_body_bytes = frame_max_body_bytes);

} // namespace siftwing::net
