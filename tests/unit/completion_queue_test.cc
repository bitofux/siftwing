/*
 * PROJECT : SIFTWING
 * FILE    : completion_queue_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 有界FIFO、拒绝保持、归属隔离与锁外陈旧结果检查
 */
#include "siftwing/concurrency/completion_queue.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <type_traits>
namespace {
using namespace siftwing::concurrency;
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool value, const char *why) {
    ++checks;
    if (!value) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
std::unique_ptr<RoutedCompletion> output(ConnectionId route, std::uint64_t id) {
    Frame frame{MessageType::search_request, 0, id, "{}"};
    auto source_result = SearchTask::create(frame, 128);
    auto source = std::get<SearchTask>(std::move(source_result));
    auto task_result = RoutedSearchTask::create(route, source);
    auto task = std::get<RoutedSearchTask>(std::move(task_result));
    auto result = RoutedCompletion::success(task, "{\"result\":" + std::to_string(id) + "}");
    return std::make_unique<RoutedCompletion>(std::get<RoutedCompletion>(std::move(result)));
} // 所有输入/任务离开后仅结果存活，验证队列不借用源载荷。
void rejects(CompletionQueue &queue,
    std::unique_ptr<RoutedCompletion> &source,
    CompletionPublishResult expected) {
    const auto *pointer = source.get();
    const auto route = source ? source->route() : ConnectionId{};
    const auto body = source && source->valid() ? source->payload()->frame()->body : "";
    const auto state = queue.status();
    require(queue.try_publish(source) == expected && source.get() == pointer,
        "rejection preserves unique owner");
    require((!source || source->route() == route)
                && (!source || !source->valid() || source->payload()->frame()->body == body),
        "rejection preserves routed payload");
    const auto after = queue.status();
    require(after.size == state.size && after.closed == state.closed, "rejection preserves queue");
}
}
static_assert(!std::is_copy_constructible_v<CompletionQueue>
              && !std::is_move_constructible_v<CompletionQueue>);
static_assert(std::is_nothrow_destructible_v<CompletionQueue>);
int main() {
    for (auto capacity : {std::size_t(0), std::size_t(1025)}) {
        bool caught = false;
        try {
            CompletionQueue invalid(0, capacity);
        } catch (const std::invalid_argument &) {
            caught = true;
        }
        require(caught, "invalid capacity");
    }
    CompletionQueue queue(0, 1), other(7, 1);
    auto state = queue.status();
    require(state.reactor_id == 0 && state.capacity == 1 && state.size == 0 && !state.closed,
        "zero owner valid and empty status");
    require(!queue.try_pop(), "open empty pop");
    std::unique_ptr<RoutedCompletion> empty;
    rejects(queue, empty, CompletionPublishResult::invalid_completion);
    auto first = output({0, 0, 1}, 77);
    const auto *pointer = first.get();
    require(queue.try_publish(first) == CompletionPublishResult::accepted && !first,
        "accepted moves exact owner");
    auto fresh = output({0, 0, 2}, 77);
    rejects(queue, fresh, CompletionPublishResult::full);
    auto wrong = output({7, 0, 1}, 77);
    rejects(queue, wrong, CompletionPublishResult::wrong_reactor); // 归属先于full。
    rejects(queue, empty, CompletionPublishResult::invalid_completion);
    auto invalid = output({0, 0, 1}, 77);
    auto moved = std::move(*invalid);
    rejects(queue, invalid, CompletionPublishResult::invalid_completion);
    require(other.try_publish(wrong) == CompletionPublishResult::accepted && !wrong,
        "other owner uses separate queue");
    auto popped = queue.try_pop();
    require(popped.get() == pointer && popped->route() == ConnectionId{0, 0, 1},
        "pop transfers exact object");
    queue.close();
    queue.close();
    rejects(queue, fresh, CompletionPublishResult::closed);
    rejects(queue, invalid, CompletionPublishResult::closed);
    rejects(queue, empty, CompletionPublishResult::closed);
    auto foreign = output({7, 0, 1}, 77);
    rejects(queue, foreign, CompletionPublishResult::closed);
    require(queue.status().closed && !queue.try_pop(), "closed empty remains observable");
    other.close();
    require(other.try_pop()->route().reactor_id == 7 && !other.try_pop(),
        "close preserves received output for drain");
    // 独立顺序参考涵盖上限容量和多轮环绕，不把取出顺序等同worker完成顺序。
    CompletionQueue ring(9, 1024);
    std::uint64_t next = 1, expected = 1;
    for (unsigned int round = 0; round < 8; ++round) {
        while (ring.status().size < 1024) {
            auto value = output({9, 2, next}, next);
            require(ring.try_publish(value) == CompletionPublishResult::accepted && !value,
                "bounded ring fill");
            ++next;
        }
        for (unsigned int i = 0; i < 513; ++i) {
            auto value = ring.try_pop();
            require(value && value->payload()->frame()->request_id == expected
                        && value->route() == ConnectionId{9, 2, expected}
                        && value->payload()->frame()->body
                               == "{\"result\":" + std::to_string(expected) + "}",
                "independent FIFO route and body oracle");
            ++expected;
        }
    }
    ring.close();
    while (auto value = ring.try_pop()) {
        require(value->payload()->frame()->request_id == expected++, "closed FIFO drain");
    }
    require(expected == next && ring.status().size == 0, "every ring result consumed once");
    // worker只核owner，陈旧条目仍占容量；所属线程取出之后才查询身份表并丢弃。
    ConnectionIdentityTable table(31, 1);
    CompletionQueue mailbox(31, 1);
    const auto old = std::get<ConnectionRegistration>(table.activate(0));
    auto delayed = output(old.identity, 77);
    require(mailbox.try_publish(delayed) == CompletionPublishResult::accepted,
        "old live result queued");
    require(table.retire(old.identity), "close old route");
    const auto now = std::get<ConnectionRegistration>(table.activate(0));
    auto current = output(now.identity, 77);
    rejects(mailbox, current, CompletionPublishResult::full);
    auto stale = mailbox.try_pop();
    require(stale && !table.matches(stale->route()) && table.matches(now.identity),
        "stale result rejected outside queue, replacement unaffected");
    stale.reset();
    require(mailbox.try_publish(current) == CompletionPublishResult::accepted && !current,
        "drain releases stale capacity");
    require(table.matches(mailbox.try_pop()->route()), "current generation survives handoff");
    auto already_expired = output(old.identity, 77);
    require(mailbox.try_publish(already_expired) == CompletionPublishResult::accepted
                && !table.matches(mailbox.try_pop()->route()),
        "queue never pretends to validate liveness");
    std::cout << "completion queue checks=" << checks << '\n';
}
