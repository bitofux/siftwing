/*
 * PROJECT : SIFTWING
 * FILE    : deadline_queue_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 独立map参考证明期限更新、取消、容量和到期顺序，Release仍执行
 */
#include "siftwing/net/deadline_queue.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        std::cerr << "deadline queue failed: " << label << '\n';
        std::exit(1);
    }
}
bool same(std::optional<DeadlineEntry> a, std::optional<DeadlineEntry> b) {
    return a.has_value() == b.has_value() && (!a || (a->key == b->key && a->when == b->when));
}
// 独立参考以key有序map存值，每次比较(when,key)，不复用被测删除或扫描实现。
std::optional<DeadlineEntry> reference_next(const std::map<std::uint64_t, DeadlineTime> &model) {
    std::optional<DeadlineEntry> result;
    for (const auto &entry : model)
        if (!result
            || std::make_pair(entry.second, entry.first)
                   < std::make_pair(result->when, result->key))
            result = DeadlineEntry{entry.first, entry.second};
    return result;
}
} // namespace
int main() {
    DeadlineQueue aggregate(65536);
    check(aggregate.capacity() == 65536 && aggregate.upsert({65536, 7}) == DeadlineUpdate::stored
              && aggregate.pop_due(7)->key == 65536 && aggregate.size() == 0,
        "aggregate capacity preserves due entry and real removal");
    for (const std::size_t bad : {std::size_t{0}, std::size_t{65537}}) {
        bool caught = false;
        try {
            DeadlineQueue queue(bad);
        } catch (const std::invalid_argument &) {
            caught = true;
        }
        check(caught, "capacity rejected before allocation");
    }
    DeadlineQueue one(1);
    check(one.size() == 0 && one.capacity() == 1 && !one.next() && !one.pop_due(1), "empty");
    check(one.upsert({0, 1}) == DeadlineUpdate::invalid
              && one.upsert({1, 0}) == DeadlineUpdate::invalid
              && one.upsert({1, -1}) == DeadlineUpdate::invalid && one.size() == 0,
        "invalid unchanged");
    check(one.upsert({1, 10}) == DeadlineUpdate::stored && !one.pop_due(9), "before boundary");
    check(one.upsert({2, 1}) == DeadlineUpdate::full && same(one.next(), DeadlineEntry{1, 10}),
        "full preserves old");
    for (DeadlineTime i = 1; i <= 10000; ++i)
        check(one.upsert({1, i}) == DeadlineUpdate::stored && one.size() == 1,
            "full updates never accumulate");
    check(!one.erase(0) && !one.erase(2) && !one.pop_due(-1),
        "unknown erase and negative now preserve");
    check(same(one.pop_due(10000), DeadlineEntry{1, 10000}) && one.size() == 0 && !one.erase(1),
        "equal boundary consumes once");
    check(one.upsert(
              {std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<DeadlineTime>::max()})
              == DeadlineUpdate::stored,
        "max key and time");
    check(same(one.pop_due(std::numeric_limits<DeadlineTime>::max()),
              DeadlineEntry{std::numeric_limits<std::uint64_t>::max(),
                  std::numeric_limits<DeadlineTime>::max()}),
        "max boundary");
    DeadlineQueue maximum(2048);
    for (std::uint64_t k = 1; k <= 2048; ++k)
        check(maximum.upsert({k, 1}) == DeadlineUpdate::stored, "maximum real storage");
    check(maximum.upsert({2049, 1}) == DeadlineUpdate::full, "maximum bounded");
    for (std::uint64_t k = 1; k <= 2048; ++k)
        check(same(maximum.pop_due(1), DeadlineEntry{k, 1}), "equal time deterministic key order");
    DeadlineQueue queue(7);
    std::map<std::uint64_t, DeadlineTime> model;
    std::uint32_t seed = 0x7319U;
    for (unsigned int n = 0; n < 12000; ++n) {
        seed = seed * 1664525U + 1013904223U;
        const std::uint64_t key = (seed >> 4U) % 13U;
        const DeadlineTime when = static_cast<DeadlineTime>((seed >> 12U) % 31U) - 1;
        switch (seed % 4U) {
            case 0: {
                const auto expected = key == 0 || when <= 0 ? DeadlineUpdate::invalid
                                      : model.count(key) || model.size() < 7
                                          ? DeadlineUpdate::stored
                                          : DeadlineUpdate::full;
                check(queue.upsert({key, when}) == expected, "model update result");
                if (expected == DeadlineUpdate::stored)
                    model[key] = when;
                break;
            }
            case 1:
                check(queue.erase(key) == (model.erase(key) != 0), "model cancel");
                break;
            case 2: {
                auto due = reference_next(model);
                if (due && due->when > when)
                    due.reset();
                check(same(queue.pop_due(when), due), "model due with <=");
                if (due)
                    model.erase(due->key);
                break;
            }
            default:
                break;
        }
        check(queue.size() == model.size() && queue.size() <= queue.capacity(), "model size");
        check(same(queue.next(), reference_next(model)), "model minimum");
    }
    std::cout << "deadline queue checks=" << checks << '\n';
}
