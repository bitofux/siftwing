/*
 * PROJECT : SIFTWING
 * FILE    : frame_codec.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 先验证再发布拥有结果，以 SAX 限制 JSON 嵌套且不生成 DOM
 */

#include "siftwing/net/frame_codec.hpp"

#include <nlohmann/json.hpp>
#include <utf8.h>

#include <stdexcept>

namespace siftwing::net {
namespace {
void check_configuration(ReceiveRole receiver, std::size_t limit) {
    if ((receiver != ReceiveRole::server && receiver != ReceiveRole::client) || limit < 2
        || limit > frame_max_body_bytes) {
        throw std::invalid_argument("invalid frame codec configuration");
    }
}

std::optional<ProtocolError> check_header(
    const Frame &frame, ReceiveRole receiver, std::size_t limit) {
    check_configuration(receiver, limit);
    bool request = false;
    switch (frame.message_type) {
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
            return ProtocolError::message_type;
    }
    if (frame.message_type == MessageType::error_response ? (frame.status < 1 || frame.status > 6)
                                                          : frame.status != 0) {
        return ProtocolError::status;
    }
    if (frame.body.size() < 2 || frame.body.size() > limit)
        return ProtocolError::body_length;
    if (frame.request_id == 0)
        return ProtocolError::request_id;
    if (request != (receiver == ReceiveRole::server))
        return ProtocolError::direction;
    return std::nullopt;
}

// SAX 只保留深度/根类别；字符串与数值由固定库验证，解析栈在超过64层时停止增长。
struct BodySax final : nlohmann::json_sax<nlohmann::json> {
    std::size_t depth = 0;
    bool object_root = false;
    bool too_deep = false;
    bool null() override {
        return true;
    }
    bool boolean(bool) override {
        return true;
    }
    bool number_integer(number_integer_t) override {
        return true;
    }
    bool number_unsigned(number_unsigned_t) override {
        return true;
    }
    bool number_float(number_float_t, const string_t &) override {
        return true;
    }
    bool string(string_t &) override {
        return true;
    }
    bool binary(binary_t &) override {
        return false;
    }
    bool start_object(std::size_t) override {
        if (depth == 0)
            object_root = true;
        return enter();
    }
    bool key(string_t &) override {
        return true;
    }
    bool end_object() override {
        --depth;
        return true;
    }
    bool start_array(std::size_t) override {
        return enter();
    }
    bool end_array() override {
        --depth;
        return true;
    }
    bool parse_error(
        std::size_t, const std::string &, const nlohmann::detail::exception &) override {
        return false;
    }
    bool enter() {
        if (depth == frame_max_json_depth) {
            too_deep = true;
            return false;
        }
        ++depth;
        return true;
    }
};

std::optional<ProtocolError> check_body(MessageType type, std::string_view body) {
    if (!utf8::is_valid(body.begin(), body.end()))
        return ProtocolError::utf8;
    // 库把原始 NUL 视为输入结束，必须先拒绝，避免 {} 后隐藏未解析字节；转义不受影响。
    if (body.find('\0') != std::string_view::npos)
        return ProtocolError::json;
    // 库允许 UTF-8 BOM，但线上 JSON 只允许语法空白；明确拒绝隐藏前缀。
    if (body.substr(0, 3) == "\xef\xbb\xbf")
        return ProtocolError::json;
    BodySax sax;
    if (!nlohmann::json::sax_parse(body.begin(), body.end(), &sax)) {
        return sax.too_deep ? ProtocolError::json_depth : ProtocolError::json;
    }
    if (!sax.object_root)
        return ProtocolError::json_object;
    if ((type == MessageType::ping || type == MessageType::pong) && body != "{}") {
        return ProtocolError::heartbeat;
    }
    return std::nullopt;
}

// 仅在字段/长度受检之后写固定偏移；不依赖主机布局，uint64 覆盖所有字段宽度。
void write_big_endian(
    std::string &bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i) {
        const auto shift = static_cast<unsigned int>((width - i - 1) * 8);
        bytes[offset + i] = static_cast<char>((value >> shift) & 0xffU);
    }
}
} // namespace

std::optional<ProtocolError> validate_frame(
    const Frame &frame, ReceiveRole receiver, std::size_t max_body_bytes) {
    if (auto error = check_header(frame, receiver, max_body_bytes))
        return error;
    return check_body(frame.message_type, frame.body);
}

EncodeResult encode_frame(const Frame &frame, ReceiveRole receiver, std::size_t max_body_bytes) {
    if (auto error = validate_frame(frame, receiver, max_body_bytes))
        return *error;
    // 协议硬上限及 decoder 的 size_t 合同排除总长溢出，输出局部拥有，异常不触输入。
    std::string bytes(frame_header_bytes + frame.body.size(), '\0');
    bytes.replace(0, 4, "SFTW");
    bytes[4] = 1;
    bytes[5] = 24;
    write_big_endian(bytes, 6, 2, static_cast<std::uint16_t>(frame.message_type));
    write_big_endian(bytes, 10, 2, frame.status);
    write_big_endian(bytes, 12, 4, frame.body.size());
    write_big_endian(bytes, 16, 8, frame.request_id);
    bytes.replace(frame_header_bytes, frame.body.size(), frame.body);
    return bytes;
}

ResponseResult make_response(
    const Frame &request, std::string_view body, std::size_t max_body_bytes) {
    if (auto error = validate_frame(request, ReceiveRole::server, max_body_bytes))
        return *error;
    MessageType type = MessageType::pong;
    switch (request.message_type) {
        case MessageType::ping:
            break;
        case MessageType::server_info_request:
            type = MessageType::server_info_response;
            break;
        case MessageType::search_request:
            type = MessageType::search_response;
            break;
        case MessageType::suggest_request:
            type = MessageType::suggest_response;
            break;
        default:
            return ProtocolError::direction;
    }
    // 响应输入也先限字节/验证，避免为已知超限/非法 Body 分配拥有副本。
    if (body.size() < 2 || body.size() > max_body_bytes)
        return ProtocolError::body_length;
    if (auto error = check_body(type, body))
        return *error;
    return Frame{type, 0, request.request_id, std::string{body}};
}

ResponseResult make_error_response(
    const Frame &request, ErrorStatus status, std::size_t max_body_bytes) {
    if (auto error = check_header(request, ReceiveRole::server, max_body_bytes))
        return *error;
    std::string_view body;
    switch (status) {
        case ErrorStatus::invalid_request:
            body = R"({"message":"invalid request"})";
            break;
        case ErrorStatus::overloaded:
            body = R"({"message":"overloaded"})";
            break;
        case ErrorStatus::stopping:
            body = R"({"message":"stopping"})";
            break;
        case ErrorStatus::internal_error:
            body = R"({"message":"internal error"})";
            break;
        case ErrorStatus::deadline_exceeded:
            body = R"({"message":"deadline exceeded"})";
            break;
        case ErrorStatus::resource_limit:
            body = R"({"message":"resource limit"})";
            break;
        default:
            return ProtocolError::status;
    }
    if (body.size() > max_body_bytes)
        return ProtocolError::body_length;
    return Frame{MessageType::error_response,
        static_cast<std::uint16_t>(status),
        request.request_id,
        std::string{body}};
}

} // namespace siftwing::net
