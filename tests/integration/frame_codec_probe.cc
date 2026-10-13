/*
 * PROJECT : SIFTWING
 * FILE    : frame_codec_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 将codec实际结果以逐行机器字段交给独立Python参考，不生成预期
 */
#include "siftwing/net/frame_codec.hpp"
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

namespace {
using namespace siftwing::net;
std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    if (bytes.empty())
        return "-";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char byte : bytes) {
        const auto c = static_cast<unsigned char>(byte);
        out += digits[c / 16];
        out += digits[c % 16];
    }
    return out;
}
unsigned int nibble(char c) {
    if (c >= '0' && c <= '9')
        return static_cast<unsigned int>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<unsigned int>(c - 'a' + 10);
    throw std::invalid_argument("invalid probe hex digit");
}
std::string unhex(const std::string &s) {
    if (s == "-")
        return {};
    if (s.size() % 2 || s.size() > 2 * (frame_header_bytes + frame_max_body_bytes + 64))
        throw std::invalid_argument("invalid probe hex size");
    std::string out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2)
        out += static_cast<char>(nibble(s[i]) * 16 + nibble(s[i + 1]));
    return out;
}
// 输出稳定原因名字，不依赖本地枚举底层值；线上status另作为整数打印。
const char *name(DecodeError e) {
    switch (e) {
#define DECODE_NAME(x)                                                                             \
    case DecodeError::x:                                                                           \
        return #x
        DECODE_NAME(magic);
        DECODE_NAME(version);
        DECODE_NAME(header_size);
        DECODE_NAME(message_type);
        DECODE_NAME(flags);
        DECODE_NAME(status);
        DECODE_NAME(body_length);
        DECODE_NAME(request_id);
        DECODE_NAME(direction);
        DECODE_NAME(buffer_limit);
#undef DECODE_NAME
    }
    throw std::logic_error("unknown decode error");
}
const char *name(ProtocolError e) {
    switch (e) {
#define PROTOCOL_NAME(x)                                                                           \
    case ProtocolError::x:                                                                         \
        return #x
        PROTOCOL_NAME(message_type);
        PROTOCOL_NAME(status);
        PROTOCOL_NAME(body_length);
        PROTOCOL_NAME(request_id);
        PROTOCOL_NAME(direction);
        PROTOCOL_NAME(utf8);
        PROTOCOL_NAME(json);
        PROTOCOL_NAME(json_object);
        PROTOCOL_NAME(json_depth);
        PROTOCOL_NAME(heartbeat);
#undef PROTOCOL_NAME
    }
    throw std::logic_error("unknown protocol error");
}
void print(EncodeResult encoded) {
    if (const auto *bytes = std::get_if<std::string>(&encoded))
        std::cout << "W " << hex(*bytes) << '\n';
    else
        std::cout << "P " << name(std::get<ProtocolError>(encoded)) << '\n';
}
} // namespace
int main() {
    // 一进程多用例避免每例spawn；输入来自原创参考测试，probe格式不是生产协议。
    std::string line;
    try {
        while (std::getline(std::cin, line)) {
            std::istringstream in(line);
            char operation = 0, role_char = 0;
            std::size_t limit = 0, budget = 0;
            std::string bytes_hex;
            in >> operation >> role_char >> limit;
            const auto role = role_char == 's' ? ReceiveRole::server : ReceiveRole::client;
            if (operation == 'D') {
                in >> budget >> bytes_hex;
                Buffer input(budget);
                const auto bytes = unhex(bytes_hex);
                if (!input.append(bytes))
                    throw std::invalid_argument("probe input exceeds buffer budget");
                const auto before = std::string(input.readable_view());
                const auto capacity = input.capacity();
                const auto view = input.readable_view();
                const auto result = decode_next(input, role, limit);
                if (const auto *frame = std::get_if<Frame>(&result)) {
                    const auto error = validate_frame(*frame, role, limit);
                    std::cout << "F " << static_cast<std::uint16_t>(frame->message_type) << ' '
                              << frame->status << ' ' << frame->request_id << ' '
                              << hex(frame->body) << ' ' << hex(input.readable_view()) << ' '
                              << (error ? name(*error) : "ok") << '\n';
                } else {
                    if (input.readable_view() != before || input.capacity() != capacity
                        || input.readable_view().data() != view.data())
                        throw std::logic_error("probe observed input mutation on wait/error");
                    if (const auto *e = std::get_if<DecodeError>(&result))
                        std::cout << "E " << name(*e) << ' ' << hex(input.readable_view()) << '\n';
                    else
                        std::cout << "N " << hex(input.readable_view()) << '\n';
                }
            } else if (operation == 'E') {
                unsigned int type = 0, status = 0;
                std::uint64_t id = 0;
                in >> type >> status >> id >> bytes_hex;
                if (type > 65535 || status > 65535)
                    throw std::invalid_argument("probe field out of range");
                print(encode_frame({static_cast<MessageType>(type),
                                       static_cast<std::uint16_t>(status),
                                       id,
                                       unhex(bytes_hex)},
                    role,
                    limit));
            } else
                throw std::invalid_argument("invalid probe operation");
            if (!in)
                throw std::invalid_argument("incomplete probe line");
        }
        if (!std::cin.eof() || !std::cout)
            return 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
    return 0;
}
