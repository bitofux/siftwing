/*
 * PROJECT : SIFTWING
 * FILE    : search_task.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 准入后拥有交接、移空状态和独立响应构造
 */
#include "siftwing/concurrency/completion.hpp"
#include <type_traits>
#include <utility>

namespace siftwing::concurrency {
namespace {
static_assert(std::is_nothrow_move_constructible_v<net::Frame>);
static_assert(std::is_nothrow_move_assignable_v<net::Frame>);
void invalidate(net::Frame &frame) noexcept {
    frame.request_id = 0;
    frame.body.clear();
}
} // namespace
SearchTask::SearchTask(net::Frame &&source, std::size_t limit) noexcept
    : request_(std::move(source)), body_limit_(limit) {
    invalidate(source);
}
std::variant<SearchTask, net::ProtocolError> SearchTask::create(
    net::Frame &source, std::size_t limit) {
    if (const auto error = net::validate_frame(source, net::ReceiveRole::server, limit))
        return *error;
    if (source.message_type != net::MessageType::search_request
        && source.message_type != net::MessageType::suggest_request)
        return net::ProtocolError::message_type;
    // 从这里起只有已证明不抛的移动；variant成功构造不会丢失提交后的拥有者。
    return SearchTask(std::move(source), limit);
}
SearchTask::SearchTask(SearchTask &&other) noexcept
    : request_(std::move(other.request_)), body_limit_(std::exchange(other.body_limit_, 0)) {
    invalidate(other.request_);
}
SearchTask &SearchTask::operator=(SearchTask &&other) noexcept {
    if (this != &other) {
        request_ = std::move(other.request_);
        body_limit_ = std::exchange(other.body_limit_, 0);
        invalidate(other.request_);
    }
    return *this;
}
bool SearchTask::valid() const noexcept {
    return request_.request_id != 0;
}
const net::Frame *SearchTask::request() const & noexcept {
    return valid() ? &request_ : nullptr;
}
std::size_t SearchTask::body_limit() const noexcept {
    return body_limit_;
}
Completion::Completion(net::Frame &&frame) noexcept : frame_(std::move(frame)) {
    invalidate(frame);
}
std::variant<Completion, net::ProtocolError> Completion::success(
    const SearchTask &task, std::string_view body) {
    if (!task.valid())
        return net::ProtocolError::request_id;
    auto result = net::make_response(*task.request(), body, task.body_limit());
    if (const auto *error = std::get_if<net::ProtocolError>(&result))
        return *error;
    return Completion(std::move(std::get<net::Frame>(result)));
}
std::variant<Completion, net::ProtocolError> Completion::error(
    const SearchTask &task, net::ErrorStatus status) {
    if (!task.valid())
        return net::ProtocolError::request_id;
    auto result = net::make_error_response(*task.request(), status, task.body_limit());
    if (const auto *error = std::get_if<net::ProtocolError>(&result))
        return *error;
    return Completion(std::move(std::get<net::Frame>(result)));
}
Completion::Completion(Completion &&other) noexcept : frame_(std::move(other.frame_)) {
    invalidate(other.frame_);
}
Completion &Completion::operator=(Completion &&other) noexcept {
    if (this != &other) {
        frame_ = std::move(other.frame_);
        invalidate(other.frame_);
    }
    return *this;
}
bool Completion::valid() const noexcept {
    return frame_.request_id != 0;
}
const net::Frame *Completion::frame() const & noexcept {
    return valid() ? &frame_ : nullptr;
}
net::Frame Completion::take_frame() && noexcept {
    auto result = std::move(frame_);
    invalidate(frame_);
    return result;
}
} // namespace siftwing::concurrency
