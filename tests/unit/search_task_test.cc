/*
 * PROJECT : SIFTWING
 * FILE    : search_task_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 请求/响应独立拥有、移动失效、准入与关联号合同
 */
#include "siftwing/concurrency/completion.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using namespace siftwing::concurrency;
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool value, const char *label) {
    ++checks;
    if (!value) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}
bool same(const Frame &a, const Frame &b) {
    return a.message_type == b.message_type && a.status == b.status && a.request_id == b.request_id
           && a.body == b.body;
}
SearchTask task(Frame &source, std::size_t limit = frame_max_body_bytes) {
    auto result = SearchTask::create(source, limit);
    require(std::holds_alternative<SearchTask>(result), "task success");
    return std::get<SearchTask>(std::move(result));
}
Completion completion(const SearchTask &request, std::string_view body) {
    auto result = Completion::success(request, body);
    require(std::holds_alternative<Completion>(result), "completion success");
    return std::get<Completion>(std::move(result));
}
void rejected(Frame source, ProtocolError expected) {
    const auto before = source;
    auto result = SearchTask::create(source);
    require(std::get<ProtocolError>(result) == expected && same(source, before),
        "reject preserves every field");
}
}
static_assert(!std::is_copy_constructible_v<SearchTask> && !std::is_copy_assignable_v<SearchTask>);
static_assert(!std::is_copy_constructible_v<Completion> && !std::is_copy_assignable_v<Completion>);
static_assert(
    !std::is_default_constructible_v<SearchTask> && !std::is_default_constructible_v<Completion>);
static_assert(std::is_nothrow_move_constructible_v<SearchTask>
              && std::is_nothrow_move_assignable_v<SearchTask>);
static_assert(std::is_nothrow_move_constructible_v<Completion>
              && std::is_nothrow_move_assignable_v<Completion>);
static_assert(std::is_nothrow_move_constructible_v<std::variant<SearchTask, ProtocolError>>);
static_assert(std::is_nothrow_move_constructible_v<std::variant<Completion, ProtocolError>>);
template <class T, class = void>
struct has_rvalue_request : std::false_type {};
template <class T>
struct has_rvalue_request<T, std::void_t<decltype(std::declval<T>().request())>> : std::true_type {
};
template <class T, class = void>
struct has_rvalue_frame : std::false_type {};
template <class T>
struct has_rvalue_frame<T, std::void_t<decltype(std::declval<T>().frame())>> : std::true_type {};
static_assert(
    !has_rvalue_request<SearchTask &&>::value && !has_rvalue_request<const SearchTask &&>::value);
static_assert(
    !has_rvalue_frame<Completion &&>::value && !has_rvalue_frame<const Completion &&>::value);
int main() {
    for (auto type : {MessageType::search_request, MessageType::suggest_request}) {
        for (const auto id : {std::uint64_t(1), std::numeric_limits<std::uint64_t>::max()}) {
            Frame input{type, 0, id, " {\"query\":\"中文\\u0000x\"} "};
            const auto original = input;
            auto request = task(input, 128);
            require(input.request_id == 0 && input.body.empty() && input.message_type == type,
                "source consumed only success");
            input.body.assign(4096, 'x');
            input.request_id = 999;
            require(request.valid() && request.body_limit() == 128
                        && same(*request.request(), original),
                "request owns exact bytes");
            // 响应Body借用只持续构造调用；结果随后独立于此字符串和请求的拥有者。
            std::string body = " {\"items\":[],\"note\":\"中文\"} ";
            const auto expected_body = body;
            auto output = completion(request, body);
            body.assign(8192, 'y');
            require(output.valid() && output.frame()->body == expected_body
                        && output.frame()->request_id == id,
                "response owns bytes and id");
            require(output.frame()->message_type
                            == (type == MessageType::search_request ? MessageType::search_response
                                                                    : MessageType::suggest_response)
                        && output.frame()->status == 0,
                "type mapping");
            const auto expected = *output.frame();
            auto moved = std::move(request);
            require(!request.valid() && request.request() == nullptr && request.body_limit() == 0,
                "task move invalidates");
            require(
                moved.valid() && same(*moved.request(), original), "task move preserves payload");
            auto *self_task = &moved;
            moved = std::move(*self_task);
            require(moved.valid() && same(*moved.request(), original), "task self move");
            request = std::move(moved);
            require(request.valid() && !moved.valid(), "task move assignment");
            require(std::get<ProtocolError>(Completion::success(moved, "{}"))
                        == ProtocolError::request_id,
                "empty task success rejected");
            require(std::get<ProtocolError>(Completion::error(moved, ErrorStatus::internal_error))
                        == ProtocolError::request_id,
                "empty task error rejected");
            auto out_moved = std::move(output);
            require(
                !output.valid() && output.frame() == nullptr && same(*out_moved.frame(), expected),
                "completion move");
            auto *self_output = &out_moved;
            out_moved = std::move(*self_output);
            require(out_moved.valid(), "completion self move");
            output = std::move(out_moved);
            require(output.valid() && !out_moved.valid(), "completion assignment");
            auto frame = std::move(output).take_frame();
            require(same(frame, expected) && !output.valid(), "take frame owns complete output");
            const auto empty = std::move(output).take_frame();
            require(empty.request_id == 0 && empty.body.empty(), "repeat take invalid");
            require(!validate_frame(frame, ReceiveRole::client, 128), "detached response admitted");
        }
    }
    rejected({MessageType::ping, 0, 7, "{}"}, ProtocolError::message_type);
    rejected({MessageType::ping, 0, 7, "{ }"}, ProtocolError::heartbeat);
    rejected({MessageType::server_info_request, 0, 7, "{}"}, ProtocolError::message_type);
    rejected({MessageType::search_response, 0, 7, "{}"}, ProtocolError::direction);
    rejected({MessageType::search_request, 1, 7, "{}"}, ProtocolError::status);
    rejected({MessageType::search_request, 0, 0, "{}"}, ProtocolError::request_id);
    rejected({MessageType::search_request, 0, 7, "[]"}, ProtocolError::json_object);
    rejected({MessageType::suggest_request, 0, 7, "{"}, ProtocolError::body_length);
    rejected({MessageType::search_request, 0, 7, std::string("{\"") + char(0xff) + "\":0}"},
        ProtocolError::utf8);
    Frame input{MessageType::search_request, 0, 7, "{}"};
    for (auto limit : {std::size_t(0), std::size_t(1), frame_max_body_bytes + 1}) {
        const auto before = input;
        bool caught = false;
        try {
            (void)SearchTask::create(input, limit);
        } catch (const std::invalid_argument &) {
            caught = true;
        }
        require(caught && same(input, before), "configuration exception preserves");
    }
    auto small = task(input, 2);
    require(
        std::get<ProtocolError>(Completion::success(small, "{ }")) == ProtocolError::body_length,
        "response shares request budget");
    require(std::get<ProtocolError>(Completion::error(small, ErrorStatus::internal_error))
                == ProtocolError::body_length,
        "small error budget no recursion");
    auto empty_result = completion(small, "{}");
    require(empty_result.valid(), "empty business result can succeed");
    Frame next{MessageType::search_request, 0, 8, "{}"};
    auto broad = task(next, 256);
    for (unsigned int code = 1; code <= 6; ++code) {
        auto result = Completion::error(broad, static_cast<ErrorStatus>(code));
        require(std::holds_alternative<Completion>(result), "known status success");
        const auto &value = std::get<Completion>(result);
        require(value.frame()->message_type == MessageType::error_response
                    && value.frame()->status == code && value.frame()->request_id == 8
                    && !validate_frame(*value.frame(), ReceiveRole::client, 256),
            "error owns valid response");
    }
    require(std::get<ProtocolError>(Completion::error(broad, static_cast<ErrorStatus>(0)))
                == ProtocolError::status,
        "unknown error status");
    for (const auto body : {"[]", "not json"}) {
        auto result = Completion::success(broad, body);
        require(std::holds_alternative<ProtocolError>(result) && broad.valid(),
            "invalid output retains task");
    }
    // 独立构造合法对象至1MiB硬端点；超端点拒绝仍保留全部源字段。
    Frame maximum{MessageType::search_request,
        0,
        9,
        "{\"x\":\"" + std::string(frame_max_body_bytes - 8, 'a') + "\"}"};
    require(maximum.body.size() == frame_max_body_bytes, "independent hard endpoint construction");
    const auto maximum_body = maximum.body;
    auto max_task = task(maximum);
    auto max_output = completion(max_task, maximum_body);
    require(max_output.frame()->body == maximum_body, "maximum response owns exact bytes");
    Frame too_large{MessageType::search_request, 0, 10, maximum_body + " "};
    rejected(too_large, ProtocolError::body_length);
    require(std::get<ProtocolError>(Completion::success(max_task, too_large.body))
                == ProtocolError::body_length,
        "maximum response limit rejects excess");
    // 已有有效目标的赋值必须替换原拥有数据；源生命周期结束后新结果仍完整。
    const auto broad_original = *broad.request();
    max_task = std::move(broad);
    require(
        !broad.valid() && same(*max_task.request(), broad_original) && max_task.body_limit() == 256,
        "assignment replaces valid target and budget");
    max_output = completion(max_task, "{\"replacement\":true}");
    require(
        max_output.frame()->request_id == 8 && max_output.frame()->body == "{\"replacement\":true}",
        "response assignment replaces valid target");
    // 数据载荷允许相同关联号；它没有连接身份，不能据此实施路由或fd复用判定。
    Frame duplicate{MessageType::suggest_request, 0, 8, "{}"};
    auto other = task(duplicate);
    require(other.request()->request_id == max_task.request()->request_id,
        "request id is not globally unique route");
    std::cout << "search task checks=" << checks << '\n';
}
