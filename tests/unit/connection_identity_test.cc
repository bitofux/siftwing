/*
 * PROJECT : SIFTWING
 * FILE    : connection_identity_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 完整身份匹配、slot复用、注册回滚与拥有包装合同
 */
#include "siftwing/concurrency/routed_payload.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
namespace {
using namespace siftwing::net;
using namespace siftwing::concurrency;
unsigned int checks = 0;
thread_local bool observe = false, fail_new = false;
thread_local unsigned int allocations = 0;
void require(bool v, const char *why) {
    ++checks;
    if (!v) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
SearchTask task(std::uint64_t id, std::size_t limit = 128) {
    Frame frame{MessageType::search_request, 0, id, "{\"query\":\"owned\"}"};
    auto result = SearchTask::create(frame, limit);
    require(std::holds_alternative<SearchTask>(result), "source task admitted");
    return std::get<SearchTask>(std::move(result));
}
RoutedSearchTask routed(ConnectionId id, SearchTask &source) {
    auto r = RoutedSearchTask::create(id, source);
    require(std::holds_alternative<RoutedSearchTask>(r), "route binds");
    return std::get<RoutedSearchTask>(std::move(r));
}
template <class T, class = void>
struct right_getter : std::false_type {};
template <class T>
struct right_getter<T, std::void_t<decltype(std::declval<T>().payload())>> : std::true_type {};
}
extern "C" void *__real__Znwm(std::size_t);
extern "C" void *__wrap__Znwm(std::size_t n) {
    if (observe) {
        ++allocations;
        if (fail_new) {
            fail_new = false;
            throw std::bad_alloc();
        }
    }
    return __real__Znwm(n);
}
static_assert(std::is_trivially_copyable_v<ConnectionId>);
static_assert(!std::is_copy_constructible_v<ConnectionIdentityTable>
              && !std::is_move_constructible_v<ConnectionIdentityTable>);
static_assert(!std::is_copy_constructible_v<RoutedSearchTask>
              && !std::is_copy_assignable_v<RoutedSearchTask>);
static_assert(!std::is_copy_constructible_v<RoutedCompletion>
              && !std::is_copy_assignable_v<RoutedCompletion>);
static_assert(std::is_nothrow_move_constructible_v<RoutedSearchTask>
              && std::is_nothrow_move_assignable_v<RoutedSearchTask>);
static_assert(std::is_nothrow_move_constructible_v<RoutedCompletion>
              && std::is_nothrow_move_assignable_v<RoutedCompletion>);
static_assert(
    !right_getter<RoutedSearchTask &&>::value && !right_getter<RoutedCompletion &&>::value);
static_assert(!right_getter<const RoutedSearchTask &&>::value
              && !right_getter<const RoutedCompletion &&>::value);
int main() {
    require(!ConnectionId{}.valid() && ConnectionId{0, 0, 1}.valid(),
        "zero owner/slot valid, zero generation invalid");
    for (auto n : {std::size_t(0), std::size_t(1025)}) {
        bool caught = false;
        try {
            ConnectionIdentityTable invalid(0, n);
        } catch (const std::invalid_argument &) {
            caught = true;
        }
        require(caught, "invalid table configuration");
    }
    allocations = 0;
    fail_new = true;
    observe = true;
    bool caught = false;
    try {
        ConnectionIdentityTable failure(0, 8);
    } catch (const std::bad_alloc &) {
        caught = true;
    }
    observe = false;
    require(caught && allocations == 1 && !fail_new, "table preallocation failure propagates");
    ConnectionIdentityTable table(7, 1024), other(8, 1024);
    require(table.capacity() == 1024 && !table.lookup(0)
                && !table.lookup(std::numeric_limits<std::uint64_t>::max()),
        "empty lookup and upper capacity");
    require(std::get<IdentityError>(table.activate(1024)) == IdentityError::invalid_slot,
        "invalid slot rejects");
    std::uint64_t expected_token = 1;
    for (std::uint64_t generation = 1; generation <= 4; ++generation) {
        for (std::size_t slot = 0; slot < table.capacity(); ++slot) {
            allocations = 0;
            observe = true;
            auto result = table.activate(slot);
            observe = false;
            const auto r = std::get<ConnectionRegistration>(result);
            require(
                allocations == 0
                    && r.identity == ConnectionId{7, static_cast<std::uint32_t>(slot), generation}
                    && r.poll_token == expected_token++,
                "independent owner/slot/generation/token oracle");
            require(table.matches(r.identity) && table.lookup(r.poll_token) == r.identity
                        && !other.matches(r.identity),
                "live exact match, cross owner rejects");
            require(std::get<IdentityError>(table.activate(slot)) == IdentityError::already_live
                        && table.lookup(r.poll_token) == r.identity,
                "live activation rejection preserves");
            auto wrong = r.identity;
            wrong.generation = 0;
            require(
                !table.retire(wrong) && table.matches(r.identity), "invalid retirement preserves");
            wrong = r.identity;
            ++wrong.generation;
            require(!table.matches(wrong) && !table.retire(wrong), "wrong generation rejects");
            wrong = r.identity;
            ++wrong.reactor_id;
            require(!table.retire(wrong), "wrong reactor rejects");
            require(table.retire(r.identity) && !table.retire(r.identity)
                        && !table.matches(r.identity) && !table.lookup(r.poll_token),
                "retire invalidates current key and is repeat safe");
        }
    }
    // ADD失败也必须retire，随后重用同slot不能复活旧结果或旧token。
    auto first = std::get<ConnectionRegistration>(table.activate(0));
    require(table.retire(first.identity), "registration rollback");
    auto second = std::get<ConnectionRegistration>(table.activate(0));
    require(second.identity.generation == first.identity.generation + 1
                && !table.retire(first.identity) && table.matches(second.identity)
                && !table.lookup(first.poll_token),
        "stale retirement never kills replacement");
    for (auto id :
        {ConnectionId{7, 1024, 1}, ConnectionId{7, 0, std::numeric_limits<std::uint64_t>::max()}})
        require(!table.matches(id), "range and forged maximum generation reject");
    auto source = task(77);
    const auto before = source.request()->body;
    require(
        std::get<RouteError>(RoutedSearchTask::create({}, source)) == RouteError::invalid_identity
            && source.valid() && source.request()->body == before,
        "invalid binding preserves source");
    allocations = 0;
    observe = true;
    auto request = routed(second.identity, source);
    observe = false;
    require(allocations == 0 && !source.valid() && request.route() == second.identity,
        "binding consumes only success without allocation");
    require(std::get<RouteError>(RoutedSearchTask::create(second.identity, source))
                == RouteError::empty_payload,
        "moved source rejects");
    auto response_result = RoutedCompletion::success(request, "{\"result\":1}");
    require(std::holds_alternative<RoutedCompletion>(response_result), "routed response succeeds");
    auto response = std::get<RoutedCompletion>(std::move(response_result));
    require(response.route() == request.route() && response.payload()->frame()->request_id == 77,
        "route and request id preserved independently");
    auto moved_request = std::move(request);
    auto moved_response = std::move(response);
    require(!request.valid() && request.route() == ConnectionId{} && !request.payload()
                && !response.valid() && !response.payload() && response.route() == ConnectionId{},
        "paired moves invalidate source");
    require(
        std::get<RouteError>(RoutedCompletion::success(request, "{}")) == RouteError::empty_payload,
        "empty routed task local error");
    require(std::get<RouteError>(RoutedCompletion::error(request, ErrorStatus::internal_error))
                == RouteError::empty_payload,
        "empty task error response rejects");
    auto *self = &moved_request;
    moved_request = std::move(*self);
    auto *self_response = &moved_response;
    moved_response = std::move(*self_response);
    require(moved_request.valid() && moved_response.valid(), "paired self moves preserve");
    auto replacement_source = task(88);
    request = routed({9, 2, 3}, replacement_source);
    request = std::move(moved_request);
    auto replacement_result = RoutedCompletion::success(request, "{}");
    response = std::get<RoutedCompletion>(std::move(replacement_result));
    response = std::move(moved_response);
    require(request.route() == second.identity && response.route() == second.identity
                && !moved_request.valid() && !moved_response.valid(),
        "valid target replacement preserves source route association");
    require(std::get<ProtocolError>(RoutedCompletion::success(request, "[]"))
                    == ProtocolError::json_object
                && request.valid(),
        "codec rejection preserves routed input");
    for (unsigned int code = 1; code <= 6; ++code) {
        auto r = RoutedCompletion::error(request, static_cast<ErrorStatus>(code));
        const auto &value = std::get<RoutedCompletion>(r);
        require(value.route() == request.route() && value.payload()->frame()->status == code,
            "error route preserved");
    }
    require(std::get<ProtocolError>(RoutedCompletion::error(request, static_cast<ErrorStatus>(0)))
                == ProtocolError::status,
        "local and protocol errors stay separate");
    require(table.retire(second.identity) && response.valid() && !table.matches(response.route()),
        "owned result remains valid after route expires");
    std::cout << "identity checks=" << checks << '\n';
}
