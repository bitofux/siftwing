/*
 * PROJECT : SIFTWING
 * FILE    : frame_codec_properties.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 任意字节下验证消费/保持/拥有/响应关联和编码反解性质
 */
#include "frame_codec_properties.hpp"
#include "siftwing/net/frame_codec.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace siftwing::testing {
namespace {
using namespace siftwing::net;
void require(bool condition, const char *label) {
    if (!condition) {
        std::cerr << "codec property failed: " << label << '\n';
        std::abort();
    }
}
bool same(const Frame &a, const Frame &b) {
    return a.message_type == b.message_type && a.status == b.status && a.request_id == b.request_id
           && a.body == b.body;
}
void frame_properties(const Frame &frame, ReceiveRole role, std::size_t limit) {
    const auto original = frame;
    const auto error = validate_frame(frame, role, limit);
    const auto encoded = encode_frame(frame, role, limit);
    require(same(frame, original), "validation and encoding preserve input");
    if (error) {
        require(std::holds_alternative<ProtocolError>(encoded)
                    && std::get<ProtocolError>(encoded) == *error,
            "validator and encoder refusal agree");
    } else {
        require(std::holds_alternative<std::string>(encoded), "valid frame has wire bytes");
        const auto &wire = std::get<std::string>(encoded);
        require(wire.size() == 24 + frame.body.size(), "wire size counts original body bytes");
        Buffer roundtrip(wire.size());
        require(roundtrip.append(wire), "complete output fits exact budget");
        const auto result = decode_next(roundtrip, role, limit);
        require(std::holds_alternative<Frame>(result) && same(std::get<Frame>(result), frame)
                    && roundtrip.readable_bytes() == 0,
            "encoding roundtrip preserves all fields");
        require(roundtrip.append(std::string(wire.size(), 'x')), "reuse decoded buffer storage");
        require(same(std::get<Frame>(result), frame),
            "decoded result owns body after storage overwrite");
    }
    if (role == ReceiveRole::server) {
        const auto response = make_response(frame, "{}", limit);
        require(same(frame, original), "response helper preserves request");
        if (error) {
            require(std::holds_alternative<ProtocolError>(response)
                        && std::get<ProtocolError>(response) == *error,
                "success response requires valid request");
        } else {
            require(std::holds_alternative<Frame>(response),
                "valid request permits empty-object reply");
            const auto &reply = std::get<Frame>(response);
            require(reply.request_id == frame.request_id && reply.status == 0
                        && !validate_frame(reply, ReceiveRole::client, limit),
                "success reply correlation");
        }
        // 错误helper有意绕过请求Body内容；硬上限能容纳全部固定诊断，不涉及错误递归。
        const auto failure = make_error_response(frame, ErrorStatus::invalid_request);
        require(same(frame, original), "error helper preserves request");
        if (frame.body.size() >= 2 && frame.request_id != 0) {
            require(std::holds_alternative<Frame>(failure),
                "trusted request header can report invalid body");
            const auto &reply = std::get<Frame>(failure);
            require(reply.request_id == frame.request_id && reply.status == 1
                        && reply.message_type == MessageType::error_response
                        && !validate_frame(reply, ReceiveRole::client),
                "error reply correlation and admission");
        }
    }
}

void stream_properties(std::string_view bytes, ReceiveRole role, std::size_t limit) {
    Buffer input(bytes.size());
    require(input.append(bytes), "whole bounded input fits buffer");
    // 最多64帧限制单次CPU工作；剩余字节允许未处理，不把有界停止当作流已读完。
    for (std::size_t count = 0; count < 64; ++count) {
        const std::string before(input.readable_view());
        const auto capacity = input.capacity();
        const auto view = input.readable_view();
        const auto result = decode_next(input, role, limit);
        const auto *frame = std::get_if<Frame>(&result);
        if (!frame) {
            require(input.readable_view() == before && input.capacity() == capacity
                        && input.readable_view().data() == view.data(),
                "wait/error preserves bytes capacity and view");
            const auto repeat = decode_next(input, role, limit);
            require(repeat.index() == result.index(), "unchanged input repeats result kind");
            if (const auto *failure = std::get_if<DecodeError>(&result)) {
                require(std::get<DecodeError>(repeat) == *failure,
                    "unchanged input repeats error reason");
            }
            break;
        }
        const auto consumed = 24 + frame->body.size();
        require(consumed <= before.size()
                    && input.readable_view() == std::string_view(before).substr(consumed),
            "success consumes exactly one complete frame");
        require(frame->body == std::string_view(before).substr(24, frame->body.size()),
            "body preserves raw bytes");
        frame_properties(*frame, role, limit);
        if (!validate_frame(*frame, role, limit)) {
            require(std::get<std::string>(encode_frame(*frame, role, limit))
                        == std::string_view(before).substr(0, consumed),
                "valid wire reencoding is byte-identical");
        }
    }
}
} // namespace

void exercise_codec(std::string_view bytes) {
    if (bytes.size() > codec_fuzz_max_bytes)
        return;
    for (const auto role : {ReceiveRole::server, ReceiveRole::client}) {
        stream_properties(bytes, role, 2);
        stream_properties(bytes, role, frame_max_body_bytes);
    }
    // 将任意输入作为Body，直接覆盖JSON/UTF8路径，避免坏magic挡住全部语义检查。
    const std::array<MessageType, 4> requests{{MessageType::ping,
        MessageType::server_info_request,
        MessageType::search_request,
        MessageType::suggest_request}};
    const auto selector = bytes.empty() ? 0U : static_cast<unsigned char>(bytes.front()) % 4U;
    const Frame request{requests[selector], 0, 0x0102030405060708ULL, std::string(bytes)};
    frame_properties(request, ReceiveRole::server, frame_max_body_bytes);
    const Frame response{MessageType::error_response, 6, request.request_id, request.body};
    frame_properties(response, ReceiveRole::client, frame_max_body_bytes);
}
} // namespace siftwing::testing
