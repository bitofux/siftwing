/*
 * PROJECT : SIFTWING
 * FILE    : id_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 验证身份值域、类型隔离、值语义、确定排序与失败诊断拥有关系
 */

#include "siftwing/base/id.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using siftwing::base::DocumentId;
using siftwing::base::Result;
using siftwing::base::SnapshotVersion;
using siftwing::base::TermId;

static_assert(!std::is_same_v<DocumentId, TermId>);
static_assert(!std::is_same_v<DocumentId, SnapshotVersion>);
static_assert(!std::is_same_v<TermId, SnapshotVersion>);
static_assert(!std::is_convertible_v<std::uint64_t, DocumentId>);
static_assert(!std::is_convertible_v<DocumentId, std::uint64_t>);
static_assert(!std::is_convertible_v<DocumentId, TermId>);

template <typename Id>
void value_contract() {
    static_assert(!std::is_default_constructible_v<Id>);
    static_assert(!std::is_constructible_v<Id, std::uint64_t>);
    static_assert(std::is_nothrow_copy_constructible_v<Id>);
    static_assert(std::is_nothrow_move_constructible_v<Id>);
    static_assert(std::is_nothrow_copy_assignable_v<Id>);
    static_assert(std::is_nothrow_move_assignable_v<Id>);
    static_assert(std::is_trivially_copyable_v<Id>);
    static_assert(std::is_same_v<decltype(std::declval<const Id&>().value()),
                                 std::uint64_t>);
    static_assert(std::is_same_v<decltype(Id::from_integer(0)), Result<Id>>);
    static_assert(noexcept(std::declval<const Id&>().value()));
    static_assert(noexcept(std::declval<Id>() == std::declval<Id>()));
}

std::size_t checks = 0;
std::size_t failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

template <typename Id, typename From>
void source_boundaries() {
    // 独立参考：标准源整数均不宽于当前平台的 uint64_t；负值不属于其数学值域。
    static_assert(std::numeric_limits<From>::digits <=
                  std::numeric_limits<std::uint64_t>::digits);
    constexpr std::array<From, 4> inputs{
        From{0}, From{1}, std::numeric_limits<From>::min(),
        std::numeric_limits<From>::max()};
    for (const auto input : inputs) {
        const bool fits = !std::is_signed_v<From> || input >= From{0};
        const auto result = Id::from_integer(input);
        check(result.has_value() == fits, "source integer mathematical membership");
        if (fits && result) {
            check(result.value().value() == static_cast<std::uint64_t>(input),
                  "successful identity preserves the complete source value");
        }
    }
}

template <typename Id>
void construction_and_ownership() {
    value_contract<Id>();
    source_boundaries<Id, char>();
    source_boundaries<Id, signed char>();
    source_boundaries<Id, unsigned char>();
    source_boundaries<Id, short>();
    source_boundaries<Id, unsigned short>();
    source_boundaries<Id, int>();
    source_boundaries<Id, unsigned int>();
    source_boundaries<Id, long>();
    source_boundaries<Id, unsigned long>();
    source_boundaries<Id, long long>();
    source_boundaries<Id, unsigned long long>();
    source_boundaries<Id, wchar_t>();
    source_boundaries<Id, char16_t>();
    source_boundaries<Id, char32_t>();

    const int constant = 7;
    volatile int volatile_input = 8;
    const auto from_const = Id::from_integer(constant);
    const auto from_volatile = Id::from_integer(volatile_input);
    check(from_const && from_const.value().value() == 7,
          "by-value deduction accepts const input without storing a reference");
    check(from_volatile && from_volatile.value().value() == 8,
          "by-value deduction copies volatile input");

    const auto zero = Id::from_integer(0);
    const auto maximum = Id::from_integer(std::numeric_limits<std::uint64_t>::max());
    check(zero && zero.value().value() == 0, "zero is a valid identity");
    check(maximum && maximum.value().value() == std::numeric_limits<std::uint64_t>::max(),
          "maximum is a valid identity, not a sentinel");
    if (!zero || !maximum) {
        return;
    }
    auto source = maximum.value();
    const auto copy = source;
    auto moved = std::move(source);
    check(source == maximum.value() && moved == copy, "moving an integer identity preserves its source");
    moved = zero.value();
    check(moved == zero.value() && source == maximum.value(), "copy assignment replaces only its target");
    moved = std::move(source);
    check(moved == maximum.value() && source == maximum.value(), "move assignment preserves its source");
    auto& alias = moved;
    moved = alias;
    check(moved == maximum.value(), "self copy assignment preserves value");
    moved = std::move(alias);
    check(moved == maximum.value(), "self move assignment preserves value");

    // 调用期间借用、失败返回后拥有：同时覆盖嵌入 NUL、源修改与局部源销毁。
    const auto failure = [] {
        std::string context{"id\0context", 10};
        auto result = Id::from_integer(-1, context);
        context.assign("changed");
        return result;
    }();
    check(!failure, "negative input returns failure before unsigned conversion");
    if (!failure) {
        check(failure.error().context == std::string{"id\0context", 10},
              "failure owns complete context after source destruction");
        check(failure.error().message == "integer conversion out of range",
              "failure preserves the checked conversion diagnostic");
    }
    const auto default_failure = Id::from_integer(std::numeric_limits<std::int64_t>::min());
    check(!default_failure && default_failure.error().context == "id.from_integer",
          "default context and signed minimum rejection");
    const auto empty_context = Id::from_integer(-1, "");
    check(!empty_context && empty_context.error().context.empty(), "empty failure context remains empty");
}

template <typename Id>
void equality() {
    const auto a = Id::from_integer(7);
    const auto b = Id::from_integer(7u);
    const auto c = Id::from_integer(8);
    if (!a || !b || !c) {
        check(false, "equality inputs must be creatable");
        return;
    }
    check(a.value() == b.value(), "equal same-type identities");
    check(!(a.value() != b.value()), "equal identities are not unequal");
    check(a.value() != c.value(), "distinct same-type identities");
    check(!(a.value() == c.value()), "distinct identities are not equal");
}

template <typename Id>
void ordering() {
    static_assert(noexcept(std::declval<Id>() < std::declval<Id>()));
    constexpr std::array<std::uint64_t, 5> ascending{
        0, 1, 7, 42, std::numeric_limits<std::uint64_t>::max()};
    // 所有有序对与原始已知数值比较，包含相等、极值与反向关系。
    for (const auto lhs : ascending) {
        for (const auto rhs : ascending) {
            const auto a = Id::from_integer(lhs);
            const auto b = Id::from_integer(rhs);
            if (!a || !b) {
                check(false, "ordering inputs must be creatable");
                continue;
            }
            check((a.value() < b.value()) == (lhs < rhs), "numeric less relation");
            check((a.value() > b.value()) == (lhs > rhs), "numeric greater relation");
            check((a.value() <= b.value()) == (lhs <= rhs), "numeric less-equal relation");
            check((a.value() >= b.value()) == (lhs >= rhs), "numeric greater-equal relation");
        }
    }
    std::vector<Id> identities;
    for (const auto input : {ascending[3], ascending[4], ascending[2], ascending[0], ascending[1]}) {
        auto result = Id::from_integer(input);
        if (!result) {
            check(false, "sort inputs must be creatable");
            return;
        }
        identities.push_back(result.value());
    }
    std::sort(identities.begin(), identities.end());
    for (std::size_t index = 0; index < ascending.size(); ++index) {
        check(identities[index].value() == ascending[index], "sorting matches an independently written sequence");
    }
}

// 仅为合同消费夹具：先检查 Result，失败不消费身份；不伪装为文档库查找或编号生成器。
Result<std::uint64_t> create_and_consume(std::int64_t input, std::size_t& consumed) {
    auto identity = DocumentId::from_integer(input, "consume document");
    if (!identity) {
        return Result<std::uint64_t>::failure(std::move(identity).error());
    }
    ++consumed;
    return Result<std::uint64_t>::success(identity.value().value());
}

void caller_boundary() {
    std::size_t consumed = 0;
    const auto failure = create_and_consume(-1, consumed);
    check(!failure && consumed == 0, "failure stops the caller before identity consumption");
    check(!failure && failure.error().context == "consume document", "caller preserves failure context");
    const auto success = create_and_consume(7, consumed);
    check(success && success.value() == 7 && consumed == 1, "caller consumes a successful typed identity once");
    const auto zero = create_and_consume(0, consumed);
    check(zero && zero.value() == 0 && consumed == 2, "zero success is distinguished from failure by Result");

    // 相同数字属于三个不同身份域；这里只显式读取数字，不跨类型比较。
    const auto document = DocumentId::from_integer(7);
    const auto term = TermId::from_integer(7);
    const auto version = SnapshotVersion::from_integer(7);
    check(document && term && version && document.value().value() == term.value().value() &&
              term.value().value() == version.value().value(),
          "different identity types can hold the same raw value");
}

}  // namespace

int main() {
    construction_and_ownership<DocumentId>();
    construction_and_ownership<TermId>();
    construction_and_ownership<SnapshotVersion>();
    equality<DocumentId>();
    equality<TermId>();
    equality<SnapshotVersion>();
    ordering<DocumentId>();
    ordering<TermId>();
    caller_boundary();
    std::cout << checks << " identity checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
