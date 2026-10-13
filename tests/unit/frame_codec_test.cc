/*
 * PROJECT : SIFTWING
 * FILE    : frame_codec_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 独立线字节、JSON准入、响应关联和异常保持的行为验证
 */

#include "siftwing/net/frame_codec.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace {
using namespace siftwing::net;
bool fail_next_allocation = false;
std::size_t checks = 0;
std::size_t failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
Frame request(MessageType type = MessageType::search_request, std::string body = "{}") {
    return {type, 0, 0x0102030405060708ULL, std::move(body)};
}
void rejected(const Frame &frame,
    ProtocolError error,
    ReceiveRole role = ReceiveRole::server,
    std::size_t limit = frame_max_body_bytes) {
    const auto before = frame.body;
    check(validate_frame(frame, role, limit) == error, "validation rejection reason");
    const auto encoded = encode_frame(frame, role, limit);
    check(
        std::holds_alternative<ProtocolError>(encoded) && std::get<ProtocolError>(encoded) == error,
        "encoding rejection has no bytes");
    check(frame.body == before, "rejection preserves input");
}
void response_error(const ResponseResult &result, ProtocolError error) {
    check(std::holds_alternative<ProtocolError>(result) && std::get<ProtocolError>(result) == error,
        "response rejection reason");
}

void encoding() {
    // 手工协议头，不用生产编解码构造期望；验证64位ID、方向编号、无额外NUL。
    const unsigned char raw[] = {0x53,
        0x46,
        0x54,
        0x57,
        1,
        24,
        0,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        2,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        0x7b,
        0x7d};
    const std::string golden(reinterpret_cast<const char *>(raw), sizeof raw);
    auto ping = request(MessageType::ping);
    auto encoded = encode_frame(ping, ReceiveRole::server);
    check(std::get<std::string>(encoded) == golden, "PING independent golden bytes");
    auto pong = std::get<Frame>(make_response(ping, "{}"));
    auto expected = golden;
    expected[6] = static_cast<char>(0x80);
    check(std::get<std::string>(encode_frame(pong, ReceiveRole::client)) == expected,
        "PONG independent golden bytes");
    ping.body.clear();
    check(std::get<std::string>(encoded) == golden && pong.body == "{}",
        "encoded bytes and response outlive mutated request");

    // 合法线头/非法Body仍先完成拆帧，然后按原ID回报；坏版本和长度不发布Frame。
    auto invalid_body_wire = golden;
    invalid_body_wire[7] = 3;
    invalid_body_wire[25] = ']';
    Buffer incoming(128);
    check(incoming.append(invalid_body_wire), "append malformed body under valid header");
    const auto malformed = std::get<Frame>(decode_next(incoming, ReceiveRole::server));
    check(validate_frame(malformed, ReceiveRole::server) == ProtocolError::json,
        "decoder completion precedes body validation");
    const auto error =
        std::get<Frame>(make_error_response(malformed, ErrorStatus::invalid_request));
    const auto error_wire = std::get<std::string>(encode_frame(error, ReceiveRole::client));
    check(incoming.append(error_wire), "append correlated encoded error");
    const auto received_error = std::get<Frame>(decode_next(incoming, ReceiveRole::client));
    check(received_error.request_id == malformed.request_id && received_error.status == 1
              && !validate_frame(received_error, ReceiveRole::client),
        "malformed-body request to client error composition");
    auto bad_header = golden;
    bad_header[4] = 2;
    check(incoming.append(bad_header), "append unsupported version");
    check(std::get<DecodeError>(decode_next(incoming, ReceiveRole::server)) == DecodeError::version
              && incoming.readable_view() == bad_header,
        "unsupported version preserves input and provides no guessed response frame");
    incoming.clear();
    bad_header = golden;
    for (std::size_t i = 12; i < 16; ++i)
        bad_header[i] = static_cast<char>(0xff);
    check(incoming.append(bad_header), "append malicious declared length");
    check(std::get<DecodeError>(decode_next(incoming, ReceiveRole::server))
                  == DecodeError::body_length
              && incoming.readable_view() == bad_header,
        "malicious declared length rejected before body waiting");

    // 长度0x010203独立写在12—15偏移，正文原样含空白/中文；上界ID为全ff。
    auto long_request = request();
    long_request.body = "{\"文\":\"字\"}";
    long_request.body.append(0x010203U - long_request.body.size(), ' ');
    long_request.request_id = std::numeric_limits<std::uint64_t>::max();
    const auto wire = std::get<std::string>(encode_frame(long_request, ReceiveRole::server));
    expected = golden.substr(0, 24);
    expected[7] = 3;
    expected[13] = 1;
    expected[14] = 2;
    expected[15] = 3;
    for (std::size_t i = 16; i < 24; ++i)
        expected[i] = static_cast<char>(0xff);
    check(wire == expected + long_request.body, "independent length and id byte offsets");
    Buffer buffer(frame_header_bytes + frame_max_body_bytes);
    check(buffer.append(wire), "encoded frame fits input buffer");
    const auto decoded = std::get<Frame>(decode_next(buffer, ReceiveRole::server));
    check(decoded.body == long_request.body && decoded.request_id == long_request.request_id
              && decoded.message_type == MessageType::search_request
              && buffer.readable_bytes() == 0,
        "encoding-decoding composition preserves raw body");
    long_request.body.assign(frame_max_body_bytes - 2, ' ');
    long_request.body += "{}";
    check(!validate_frame(long_request, ReceiveRole::server), "hard body limit inclusive");
    check(std::get<std::string>(encode_frame(long_request, ReceiveRole::server)).size()
              == frame_header_bytes + frame_max_body_bytes,
        "maximum encoded size");
    long_request.body += ' ';
    rejected(long_request, ProtocolError::body_length);
}

void fields_and_bodies() {
    auto frame = request();
    frame.message_type = static_cast<MessageType>(0x8005);
    rejected(frame, ProtocolError::message_type);
    frame = request();
    frame.status = 1;
    rejected(frame, ProtocolError::status);
    frame = request();
    frame.request_id = 0;
    rejected(frame, ProtocolError::request_id);
    frame = request();
    rejected(frame, ProtocolError::direction, ReceiveRole::client);
    rejected(request(MessageType::search_request, ""), ProtocolError::body_length);
    rejected(request(MessageType::search_request, " "), ProtocolError::body_length);
    rejected(request(MessageType::search_request, " {}"),
        ProtocolError::body_length,
        ReceiveRole::server,
        2);
}

void json_bodies() {
    // 原创恶意字节与语法例子分别覆盖UTF-8、JSON、根类型、严格心跳和本地解析预算。
    const std::array<std::string, 6> bad_utf8{{std::string("{\"x\":\"\xc0\xaf\"}"),
        std::string("{\"x\":\"\xed\xa0\x80\"}"),
        std::string("{\"x\":\"\xf4\x90\x80\x80\"}"),
        std::string("{\"x\":\"\x80\"}"),
        std::string("{\"x\":\"\xe4\xb8\"}"),
        std::string("{\"x\":\"\xff\"}")}};
    for (const auto &body : bad_utf8)
        rejected(request(MessageType::search_request, body), ProtocolError::utf8);
    const std::array<std::string, 15> bad_json{{"{",
        "{]",
        "{}{}",
        "{}x",
        "{\"a\":}",
        "{\"a\":01}",
        "{\"a\":NaN}",
        "{\"a\":1e999}",
        "{\"a\":\"\\uD800\"}",
        "{\"a\":\"\\uDC00\"}",
        "\xef\xbb\xbf{}",
        std::string("{}\0", 3),
        std::string("{}\0garbage", 10),
        std::string("\0{}", 3),
        std::string("{\"a\":\0null}", 11)}};
    for (const auto &body : bad_json) {
        rejected(request(MessageType::search_request, body),
            body.size() < 2 ? ProtocolError::body_length : ProtocolError::json);
    }
    for (const auto &body : {"[]", "null", "true", "12", "\"abc\""})
        rejected(request(MessageType::search_request, body), ProtocolError::json_object);
    for (const auto &body : {" {}", "{ }", "{\"x\":1}"})
        rejected(request(MessageType::ping, body), ProtocolError::heartbeat);
    for (const auto &body : {"{}",
             " \n{\"x\":[null,true,false,1,-2,1.5,\"中文\",\"\\u0000\",\"\\uD83D\\uDE00\"]}\t",
             "{\"x\":1,\"x\":2}",
             "{\"x\":18446744073709551616}"})
        check(!validate_frame(request(MessageType::search_request, body), ReceiveRole::server),
            "valid object including escapes, whitespace and unvalidated duplicate keys");
    std::string nested = "{\"x\":";
    nested.append(63, '[');
    nested += '0';
    nested.append(63, ']');
    nested += '}';
    check(!validate_frame(request(MessageType::search_request, nested), ReceiveRole::server),
        "64 container levels inclusive");
    nested.insert(5, "[");
    nested.insert(nested.size() - 1, "]");
    rejected(request(MessageType::search_request, nested), ProtocolError::json_depth);
    auto local = request(MessageType::search_request, " {} ");
    check(!validate_frame(local, ReceiveRole::server, 4), "local limit inclusive");
    rejected(local, ProtocolError::body_length, ReceiveRole::server, 3);
}

void responses() {
    std::string borrowed_body = "{\"ok\":true}";
    const auto owned_response = std::get<Frame>(make_response(request(), borrowed_body));
    borrowed_body.clear();
    check(owned_response.body == "{\"ok\":true}", "response copies borrowed body before return");
    const std::array<MessageType, 4> types{{MessageType::ping,
        MessageType::server_info_request,
        MessageType::search_request,
        MessageType::suggest_request}};
    const std::array<MessageType, 4> replies{{MessageType::pong,
        MessageType::server_info_response,
        MessageType::search_response,
        MessageType::suggest_response}};
    for (std::size_t i = 0; i < types.size(); ++i) {
        auto input = request(types[i]);
        auto output = std::get<Frame>(make_response(input, "{}"));
        check(output.message_type == replies[i] && output.status == 0
                  && output.request_id == input.request_id && output.body == "{}",
            "four explicit response mappings");
        input.body.assign("changed");
        check(output.body == "{}", "response owns body independently");
    }
    response_error(make_response(request(MessageType::ping), "{ }"), ProtocolError::heartbeat);
    response_error(make_response(request(), "[]"), ProtocolError::json_object);
    response_error(make_response(request(), " {} ", 3), ProtocolError::body_length);
    auto bad = request(MessageType::search_request, "{]");
    response_error(make_response(bad, "{}"), ProtocolError::json);
    for (std::uint16_t status = 1; status <= 6; ++status) {
        auto error = std::get<Frame>(make_error_response(bad, static_cast<ErrorStatus>(status)));
        check(error.message_type == MessageType::error_response && error.status == status
                  && error.request_id == bad.request_id,
            "errors correlate invalid body request");
        check(
            !validate_frame(error, ReceiveRole::client), "fixed diagnostic is valid JSON response");
        const auto wire = std::get<std::string>(encode_frame(error, ReceiveRole::client));
        check(static_cast<unsigned char>(wire[6]) == 255
                  && static_cast<unsigned char>(wire[7]) == 255 && wire[10] == 0
                  && static_cast<unsigned char>(wire[11]) == status,
            "error type/status wire bytes");
        for (const auto invalid : {std::uint16_t{0}, std::uint16_t{7}, std::uint16_t{65535}}) {
            error.status = invalid;
            rejected(error, ProtocolError::status, ReceiveRole::client);
        }
    }
    auto e = std::get<Frame>(make_error_response(bad, ErrorStatus::invalid_request));
    check(e.body == "{\"message\":\"invalid request\"}", "fixed error diagnostic bytes");
    response_error(
        make_error_response(bad, ErrorStatus::invalid_request, 2), ProtocolError::body_length);
    check(std::holds_alternative<Frame>(
              make_error_response(bad, ErrorStatus::invalid_request, e.body.size())),
        "error body exact budget inclusive");
    response_error(make_error_response(bad, static_cast<ErrorStatus>(0)), ProtocolError::status);
    bad.request_id = 0;
    response_error(
        make_error_response(bad, ErrorStatus::invalid_request), ProtocolError::request_id);
    response_error(
        make_error_response({MessageType::pong, 0, 1, "{}"}, ErrorStatus::invalid_request),
        ProtocolError::direction);
    auto complete = request(MessageType::search_request, std::string("{}\0", 3));
    check(
        std::holds_alternative<Frame>(make_error_response(complete, ErrorStatus::invalid_request)),
        "error helper intentionally skips malformed body");
}

void exceptions() {
    auto input = request();
    for (const auto limit : {std::size_t{0}, std::size_t{1}, frame_max_body_bytes + 1}) {
        bool threw = false;
        try {
            (void)encode_frame(input, ReceiveRole::server, limit);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        check(threw && input.body == "{}", "invalid encoder budget throws and preserves input");
        threw = false;
        try {
            (void)make_error_response(input, ErrorStatus::overloaded, limit);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        check(threw, "invalid helper budget throws");
    }
    bool threw = false;
    try {
        (void)validate_frame(input, static_cast<ReceiveRole>(42));
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw, "unknown receiver throws");
    input.body = "{\"data\":\"" + std::string(256, 'x') + "\"}";
    const auto original = input.body;
    fail_next_allocation = true;
    threw = false;
    try {
        (void)encode_frame(input, ReceiveRole::server);
    } catch (const std::bad_alloc &) {
        threw = true;
    }
    fail_next_allocation = false;
    check(threw && input.body == original && input.request_id == 0x0102030405060708ULL,
        "encoder allocation failure propagates and preserves input");
    fail_next_allocation = true;
    threw = false;
    try {
        (void)make_error_response(input, ErrorStatus::invalid_request);
    } catch (const std::bad_alloc &) {
        threw = true;
    }
    fail_next_allocation = false;
    check(threw && input.body == original, "error output allocation failure preserves request");
}
} // namespace

// 定点拒绝下一次普通分配，与真实系统OOM区分；malloc/free配对可供ASan检查。
void *operator new(std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc{};
    }
    if (void *p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc{};
}
void *operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}

int main() {
    encoding();
    fields_and_bodies();
    json_bodies();
    responses();
    exceptions();
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
