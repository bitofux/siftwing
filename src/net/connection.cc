/*
 * PROJECT : SIFTWING
 * FILE    : connection.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 有界收发及单响应关联，以前缀消费保留短写后缀
 */
#include "siftwing/net/connection.hpp"
#include "siftwing/net/socket_ops.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace siftwing::net {
namespace {
std::size_t input_limit(const UniqueFd &socket, std::size_t body_limit) {
    if (!socket || body_limit < 2 || body_limit > frame_max_body_bytes)
        throw std::invalid_argument("invalid connection configuration");
    return frame_header_bytes + body_limit;
}
static_assert(std::is_nothrow_move_constructible_v<ReadResult>,
    "publishing a request after consuming input must not throw");
// 逐个显式映射已定义类型，不按高位猜测未知协议编号。
MessageType response_type(MessageType request) noexcept {
    switch (request) {
        case MessageType::ping:
            return MessageType::pong;
        case MessageType::server_info_request:
            return MessageType::server_info_response;
        case MessageType::search_request:
            return MessageType::search_response;
        case MessageType::suggest_request:
            return MessageType::suggest_response;
        default:
            return MessageType::error_response; // 只有经解码交付的请求会进入本层。
    }
}
} // namespace

Connection::Connection(UniqueFd &&socket, std::size_t max_body_bytes, std::size_t initial_capacity)
    : input_(input_limit(socket, max_body_bytes), initial_capacity),
      output_(frame_header_bytes + max_body_bytes), socket_(std::move(socket)),
      max_body_bytes_(max_body_bytes) {}

void Connection::terminate(
    ReadStop reason, std::optional<DecodeError> header, std::error_code error) noexcept {
    terminal_ = reason;
    terminal_header_ = header;
    terminal_error_ = error;
    pending_ = false;
    response_queued_ = false;
    body_rejected_ = false;
    pending_id_ = 0;
    write_terminal_ = WriteStop::closed;
    input_.clear();
    output_.clear();
    (void)socket_.close();
}
std::error_code Connection::close() noexcept {
    if (!socket_)
        return {}; // 已终止查询保留首因，不把原错误改成显式close。
    terminal_ = ReadStop::closed;
    terminal_header_.reset();
    terminal_error_.clear();
    pending_ = false;
    response_queued_ = false;
    body_rejected_ = false;
    pending_id_ = 0;
    write_terminal_ = WriteStop::closed;
    input_.clear();
    output_.clear();
    return socket_.close();
}
bool Connection::finish_request() noexcept {
    if (!socket_ || !pending_ || output_.readable_bytes() != 0)
        return false;
    pending_ = false;
    response_queued_ = false;
    body_rejected_ = false;
    pending_id_ = 0;
    return true;
}

ReadResult Connection::read_request(std::size_t byte_budget, std::size_t attempt_budget) {
    ReadResult result;
    if (!socket_) {
        result.stop = terminal_;
        result.header_error = terminal_header_;
        result.error = terminal_error_;
        return result;
    }
    if (pending_) {
        result.stop = ReadStop::request_pending;
        return result;
    }
    if (byte_budget == 0 || attempt_budget == 0) {
        result.stop = ReadStop::invalid_budget;
        result.error = {EINVAL, std::generic_category()};
        return result;
    }
    try {
        std::array<char, 4096> chunk{};
        for (;;) {
            // 缓存帧优先：不依赖新内核就绪；预算末次读取后也必须检查头和完整帧。
            auto decoded = decode_next(input_, ReceiveRole::server, max_body_bytes_);
            if (auto *frame = std::get_if<Frame>(&decoded)) {
                result.body_error = validate_frame(*frame, ReceiveRole::server, max_body_bytes_);
                result.request.emplace(std::move(*frame));
                result.stop = ReadStop::request;
                pending_ = true;
                pending_id_ = result.request->request_id;
                pending_type_ = result.request->message_type;
                body_rejected_ = result.body_error.has_value();
                return result;
            }
            if (auto *error = std::get_if<DecodeError>(&decoded)) {
                result.stop = ReadStop::header_error;
                result.header_error = *error;
                terminate(result.stop, result.header_error);
                return result;
            }
            if (result.attempts == attempt_budget || result.bytes == byte_budget)
                return result;
            const auto capacity = std::min({chunk.size(),
                byte_budget - result.bytes,
                input_.max_bytes() - input_.readable_bytes()});
            // 完整一帧能放入Buffer；合法头+满Buffer必可解码，非法头已在上方终止。
            if (capacity == 0)
                throw std::logic_error("connection input capacity invariant violated");
            const auto io = receive_some(socket_.get(), chunk.data(), capacity);
            ++result.attempts;
            if (io.state == IoState::progress) {
                result.bytes += io.bytes;
                if (!input_.append(std::string_view(chunk.data(), io.bytes)))
                    throw std::logic_error("connection input invariant violated");
                continue;
            }
            if (io.state == IoState::interrupted)
                continue;
            if (io.state == IoState::would_block) {
                result.stop = ReadStop::would_block;
                result.error = io.error;
                return result;
            }
            if (io.state == IoState::eof) {
                result.stop =
                    input_.readable_bytes() == 0 ? ReadStop::peer_eof : ReadStop::truncated;
            } else {
                result.stop = ReadStop::io_error;
                result.error = io.error;
            }
            terminate(result.stop, {}, result.error);
            return result;
        }
    } catch (...) {
        // recv取得的字节无法退回内核；异常不可留下一个悄悄丢失前缀、仍可读取的连接。
        terminate(ReadStop::local_failure);
        throw;
    }
}

QueueResult Connection::queue_response(const Frame &response) {
    if (!socket_)
        return {QueueStop::closed};
    if (!pending_)
        return {QueueStop::no_request};
    if (response_queued_)
        return {QueueStop::response_pending};
    if (response.request_id != pending_id_
        || (response.message_type != MessageType::error_response
            && (body_rejected_ || response.message_type != response_type(pending_type_))))
        return {QueueStop::request_mismatch};
    auto encoded = encode_frame(response, ReceiveRole::client, max_body_bytes_);
    if (auto *error = std::get_if<ProtocolError>(&encoded))
        return {QueueStop::protocol_error, *error};
    // Buffer的强保证使编码或复制失败可重试；本路径尚未send，无流字节需要回滚。
    if (!output_.append(std::get<std::string>(encoded)))
        throw std::logic_error("connection output capacity invariant violated");
    response_queued_ = true;
    return {QueueStop::queued};
}

WriteResult Connection::flush_output(std::size_t byte_budget, std::size_t attempt_budget) noexcept {
    WriteResult result;
    if (!socket_) {
        result.stop = write_terminal_;
        result.error = terminal_error_;
        return result;
    }
    if (output_.readable_bytes() == 0)
        return result;
    if (byte_budget == 0 || attempt_budget == 0) {
        result.stop = WriteStop::invalid_budget;
        result.error = {EINVAL, std::generic_category()};
        return result;
    }
    for (;;) {
        // 只消费send确认的前缀；末次send排空必须优先关闭写意向。
        if (output_.readable_bytes() == 0)
            return result;
        if (result.bytes == byte_budget || result.attempts == attempt_budget) {
            result.stop = WriteStop::budget_exhausted;
            return result;
        }
        const auto bytes = output_.readable_view().substr(0, byte_budget - result.bytes);
        const auto io = send_some(socket_.get(), bytes);
        ++result.attempts;
        if (io.state == IoState::progress && io.bytes != 0 && io.bytes <= bytes.size()) {
            (void)output_.consume(io.bytes); // bytes来源于当前可读区，边界已检查。
            result.bytes += io.bytes;
            continue;
        }
        if (io.state == IoState::interrupted)
            continue;
        if (io.state == IoState::would_block) {
            result.stop = WriteStop::would_block;
            result.error = io.error;
            return result;
        }
        result.stop = WriteStop::io_error;
        result.error =
            io.state == IoState::error ? io.error : std::error_code(EIO, std::generic_category());
        terminate(ReadStop::io_error, {}, result.error);
        write_terminal_ = WriteStop::io_error;
        return result;
    }
}
} // namespace siftwing::net
