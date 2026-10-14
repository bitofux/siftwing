/*
 * PROJECT : SIFTWING
 * FILE    : routed_payload.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 身份/载荷成对移动、局部错误分支与独立拥有结果
 */
#include "siftwing/concurrency/routed_payload.hpp"
#include <utility>
namespace siftwing::concurrency {
RoutedSearchTask::RoutedSearchTask(net::ConnectionId route, SearchTask &&source) noexcept
    : route_(route), payload_(std::move(source)) {}
std::variant<RoutedSearchTask, RouteError> RoutedSearchTask::create(
    net::ConnectionId route, SearchTask &source) noexcept {
    if (!route.valid())
        return RouteError::invalid_identity;
    if (!source.valid())
        return RouteError::empty_payload;
    return RoutedSearchTask(route, std::move(source));
}
RoutedSearchTask::RoutedSearchTask(RoutedSearchTask &&other) noexcept
    : route_(std::exchange(other.route_, {})), payload_(std::move(other.payload_)) {}
RoutedSearchTask &RoutedSearchTask::operator=(RoutedSearchTask &&other) noexcept {
    if (this != &other) {
        payload_ = std::move(other.payload_);
        route_ = std::exchange(other.route_, {});
    }
    return *this;
}
bool RoutedSearchTask::valid() const noexcept {
    return route_.valid() && payload_.valid();
}
net::ConnectionId RoutedSearchTask::route() const noexcept {
    return route_;
}
const SearchTask *RoutedSearchTask::payload() const & noexcept {
    return valid() ? &payload_ : nullptr;
}
RoutedCompletion::RoutedCompletion(net::ConnectionId route, Completion &&source) noexcept
    : route_(route), payload_(std::move(source)) {}
std::variant<RoutedCompletion, RouteError, net::ProtocolError> RoutedCompletion::success(
    const RoutedSearchTask &task, std::string_view body) {
    if (!task.valid())
        return RouteError::empty_payload;
    auto result = Completion::success(*task.payload(), body);
    if (auto *error = std::get_if<net::ProtocolError>(&result))
        return *error;
    return RoutedCompletion(task.route(), std::move(std::get<Completion>(result)));
}
std::variant<RoutedCompletion, RouteError, net::ProtocolError> RoutedCompletion::error(
    const RoutedSearchTask &task, net::ErrorStatus status) {
    if (!task.valid())
        return RouteError::empty_payload;
    auto result = Completion::error(*task.payload(), status);
    if (auto *error = std::get_if<net::ProtocolError>(&result))
        return *error;
    return RoutedCompletion(task.route(), std::move(std::get<Completion>(result)));
}
RoutedCompletion::RoutedCompletion(RoutedCompletion &&other) noexcept
    : route_(std::exchange(other.route_, {})), payload_(std::move(other.payload_)) {}
RoutedCompletion &RoutedCompletion::operator=(RoutedCompletion &&other) noexcept {
    if (this != &other) {
        payload_ = std::move(other.payload_);
        route_ = std::exchange(other.route_, {});
    }
    return *this;
}
bool RoutedCompletion::valid() const noexcept {
    return route_.valid() && payload_.valid();
}
net::ConnectionId RoutedCompletion::route() const noexcept {
    return route_;
}
const Completion *RoutedCompletion::payload() const & noexcept {
    return valid() ? &payload_ : nullptr;
}
} // namespace siftwing::concurrency
