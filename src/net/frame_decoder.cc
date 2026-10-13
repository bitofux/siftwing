/*
 * PROJECT : SIFTWING
 * FILE    : frame_decoder.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 逐字节读取大端头，验证后复制完整 Body，再提交前缀消费
 */

#include "siftwing/net/frame_decoder.hpp"

#include <climits>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace siftwing::net {
namespace {
static_assert(CHAR_BIT == 8, "protocol requires eight-bit bytes");
static_assert(std::is_nothrow_move_constructible_v<DecodeResult>,
    "returning a prepared result must not throw after consuming input");
static_assert(std::numeric_limits<std::size_t>::max() >= frame_max_body_bytes + frame_header_bytes,
    "size_t must represent a maximum frame");

// 所有调用都在满 24 字节后，偏移固定且不越界；先转 unsigned char 避免有符号 char 扩展。
std::uint64_t read_big_endian(
    std::string_view bytes, std::size_t offset, std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[offset + i]);
    }
    return value;
}
} // namespace

DecodeResult decode_next(Buffer &input, ReceiveRole role, std::size_t max_body_bytes) {
    if (max_body_bytes < 2 || max_body_bytes > frame_max_body_bytes
        || (role != ReceiveRole::server && role != ReceiveRole::client)) {
        throw std::invalid_argument("invalid frame decoder configuration");
    }
    const auto bytes = input.readable_view();
    if (bytes.size() < frame_header_bytes) {
        return std::monostate{};
    }
    // 协议顺序决定多处损坏时的唯一拒绝原因；不能提前等待 Body 或分配。
    if (bytes.substr(0, 4) != "SFTW") {
        return DecodeError::magic;
    }
    if (static_cast<unsigned char>(bytes[4]) != 1) {
        return DecodeError::version;
    }
    if (static_cast<unsigned char>(bytes[5]) != frame_header_bytes) {
        return DecodeError::header_size;
    }
    const auto type = static_cast<MessageType>(read_big_endian(bytes, 6, 2));
    bool request = false;
    switch (type) {
        case MessageType::ping:
        case MessageType::server_info_request:
        case MessageType::search_request:
        case MessageType::suggest_request:
            request = true;
            break;
        case MessageType::pong:
        case MessageType::server_info_response:
        case MessageType::search_response:
        case MessageType::suggest_response:
        case MessageType::error_response:
            break;
        default:
            return DecodeError::message_type;
    }
    if (read_big_endian(bytes, 8, 2) != 0) {
        return DecodeError::flags;
    }
    const auto status = static_cast<std::uint16_t>(read_big_endian(bytes, 10, 2));
    if (type == MessageType::error_response ? (status < 1 || status > 6) : status != 0) {
        return DecodeError::status;
    }
    const auto length = read_big_endian(bytes, 12, 4);
    if (length < 2 || length > max_body_bytes) {
        return DecodeError::body_length;
    }
    const auto id = read_big_endian(bytes, 16, 8);
    if (id == 0) {
        return DecodeError::request_id;
    }
    if (request != (role == ReceiveRole::server)) {
        return DecodeError::direction;
    }
    // 上面的协议上限和 size_t 静态合同已排除窄加法回绕。额外存储拒绝在协议检查之后。
    const auto total = frame_header_bytes + static_cast<std::size_t>(length);
    if (total > input.max_bytes()) {
        return DecodeError::buffer_limit;
    }
    if (bytes.size() < total) {
        return std::monostate{};
    }
    DecodeResult result{Frame{type,
        status,
        id,
        std::string{bytes.substr(frame_header_bytes, static_cast<std::size_t>(length))}}};
    // 已拥有 Body，且返回移动由 static_assert 保证无抛出；不发布部分结果或部分消费。
    (void)input.consume(total);
    return result;
}

} // namespace siftwing::net
