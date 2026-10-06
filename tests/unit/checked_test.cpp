/*
 * PROJECT : SIFTWING
 * FILE    : checked_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-06
 * BRIEF   : 验证受检整数运算的边界、符号组合、独立数学参考与错误所有权
 */

#include "siftwing/base/checked.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace {

using siftwing::base::Result;
using siftwing::base::checked_add;
using siftwing::base::checked_mul;
using siftwing::base::checked_narrow;

static_assert(std::is_same_v<decltype(checked_add(std::size_t{}, std::size_t{})),
                             Result<std::size_t>>);
static_assert(std::is_same_v<decltype(checked_narrow<std::uint32_t>(std::uint64_t{})),
                             Result<std::uint32_t>>);

std::size_t checks = 0;
std::size_t failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        if (failures < 20) {
            std::cerr << "FAIL: " << label << '\n';
        }
        ++failures;
    }
}

template <typename T>
void expect_value(const Result<T>& result, T expected, const char* label) {
    check(result.has_value() && result.value() == expected, label);
}

template <typename T>
void exhaustive_arithmetic() {
    // 独立参考：8 位输入的和与积都能由 int 精确表示，因此 oracle 不重复生产 guard 算法。
    constexpr int minimum = std::numeric_limits<T>::min();
    constexpr int maximum = std::numeric_limits<T>::max();
    for (int lhs = minimum; lhs <= maximum; ++lhs) {
        for (int rhs = minimum; rhs <= maximum; ++rhs) {
            const auto a = static_cast<T>(lhs);
            const auto b = static_cast<T>(rhs);
            const auto sum = checked_add(a, b);
            const int expected_sum = lhs + rhs;
            const bool sum_fits = expected_sum >= minimum && expected_sum <= maximum;
            check(sum.has_value() == sum_fits &&
                      (!sum_fits || static_cast<int>(sum.value()) == expected_sum),
                  "8-bit addition vs independent int oracle");
            const auto product = checked_mul(a, b);
            const int expected_product = lhs * rhs;
            const bool product_fits = expected_product >= minimum && expected_product <= maximum;
            check(product.has_value() == product_fits &&
                      (!product_fits || static_cast<int>(product.value()) == expected_product),
                  "8-bit multiplication vs independent int oracle");
        }
    }
}

template <typename To, typename From>
void exhaustive_conversion() {
    // int 同时覆盖这里所有 8 位源/目标值域，可直接判断成员关系而不复用生产转换。
    constexpr int minimum = std::numeric_limits<From>::min();
    constexpr int maximum = std::numeric_limits<From>::max();
    constexpr int target_min = std::numeric_limits<To>::min();
    constexpr int target_max = std::numeric_limits<To>::max();
    for (int input = minimum; input <= maximum; ++input) {
        const auto result = checked_narrow<To>(static_cast<From>(input));
        const bool fits = input >= target_min && input <= target_max;
        check(result.has_value() == fits && (!fits || static_cast<int>(result.value()) == input),
              "8-bit conversion vs independent int oracle");
    }
}

// 记录布局验收夹具证明调用者按“乘法→加法→窄化”短路；不写文件、不冻结生产格式。
Result<std::uint32_t> layout_offset(std::uint64_t count, std::uint64_t width,
                                    std::uint64_t offset) {
    const auto bytes = checked_mul(count, width, "layout bytes");
    if (!bytes) {
        return Result<std::uint32_t>::failure(bytes.error());
    }
    const auto end = checked_add(offset, bytes.value(), "layout end");
    if (!end) {
        return Result<std::uint32_t>::failure(end.error());
    }
    return checked_narrow<std::uint32_t>(end.value(), "layout field");
}

void wide_boundaries() {
    // 直接覆盖最值相邻点、零、min/-1 和全部 signedness 转换方向。
    using U = std::uint64_t;
    using S = std::int64_t;
    constexpr U umax = std::numeric_limits<U>::max();
    constexpr S smax = std::numeric_limits<S>::max();
    constexpr S smin = std::numeric_limits<S>::min();

    expect_value(checked_add(umax, U{0}), umax, "unsigned max plus zero");
    expect_value(checked_add(umax - U{1}, U{1}), umax, "unsigned exact max");
    check(!checked_add(umax, U{1}), "unsigned addition overflow");
    expect_value(checked_mul(umax, U{0}), U{0}, "unsigned max times zero");
    expect_value(checked_mul(U{0}, umax), U{0}, "unsigned zero times max");
    expect_value(checked_mul(umax, U{1}), umax, "unsigned max times one");
    expect_value(checked_mul(umax / U{2}, U{2}), umax - U{1}, "unsigned last even product");
    check(!checked_mul(umax / U{2} + U{1}, U{2}), "unsigned first overflowing product");
    expect_value(checked_add(smax, S{0}), smax, "signed max plus zero");
    expect_value(checked_add(smin, S{0}), smin, "signed min plus zero");
    expect_value(checked_add(smax - S{1}, S{1}), smax, "signed exact max");
    expect_value(checked_add(smin + S{1}, S{-1}), smin, "signed exact min");
    expect_value(checked_add(smin, smax), S{-1}, "opposite signed limits");
    expect_value(checked_add(smax, smin), S{-1}, "opposite limits reversed");
    check(!checked_add(smax, S{1}), "signed addition above max");
    check(!checked_add(smin, S{-1}), "signed addition below min");
    expect_value(checked_mul(smin, S{0}), S{0}, "signed min times zero");
    expect_value(checked_mul(S{0}, smin), S{0}, "signed zero times min");
    expect_value(checked_mul(smin, S{1}), smin, "signed min times one");
    expect_value(checked_mul(smax, S{1}), smax, "signed max times one");
    expect_value(checked_mul(S{-1}, smax), -smax, "negative times positive limit");
    expect_value(checked_mul(-smax, S{-1}), smax, "negative times negative exact max");
    expect_value(checked_mul(smin / S{2}, S{2}), smin, "negative times positive exact min");
    expect_value(checked_mul(S{2}, smin / S{2}), smin, "positive times negative exact min");
    check(!checked_mul(smin, S{-1}), "min times minus one rejected before UB");
    check(!checked_mul(S{-1}, smin), "minus one times min rejected");
    check(!checked_mul(smax, S{2}), "positive product above max");
    check(!checked_mul(smin, S{2}), "negative product below min");
    check(!checked_mul(S{2}, smin), "negative product below min reversed");
    check(!checked_mul(smin, smin), "two minimum values overflow");

    expect_value(checked_narrow<U>(umax), umax, "unsigned identity");
    expect_value(checked_narrow<S>(smin), smin, "signed identity min");
    expect_value(checked_narrow<S>(smax), smax, "signed identity max");
    expect_value(checked_narrow<U>(smax), static_cast<U>(smax), "same-width signed to unsigned");
    check(!checked_narrow<U>(S{-1}), "negative to unsigned despite destination width");
    check(!checked_narrow<U>(smin), "signed minimum to unsigned");
    expect_value(checked_narrow<S>(static_cast<U>(smax)), smax, "unsigned to signed exact max");
    check(!checked_narrow<S>(static_cast<U>(smax) + U{1}), "unsigned to signed max plus one");
    check(!checked_narrow<S>(umax), "unsigned max to signed");

    constexpr auto u32max = std::numeric_limits<std::uint32_t>::max();
    constexpr auto s32max = std::numeric_limits<std::int32_t>::max();
    constexpr auto s32min = std::numeric_limits<std::int32_t>::min();
    expect_value(checked_narrow<std::uint32_t>(U{u32max}), u32max, "unsigned narrowing exact max");
    check(!checked_narrow<std::uint32_t>(U{u32max} + U{1}), "unsigned narrowing max plus one");
    expect_value(checked_narrow<std::int32_t>(S{s32min}), s32min, "signed narrowing exact min");
    expect_value(checked_narrow<std::int32_t>(S{s32max}), s32max, "signed narrowing exact max");
    check(!checked_narrow<std::int32_t>(S{s32min} - S{1}), "signed narrowing below min");
    check(!checked_narrow<std::int32_t>(S{s32max} + S{1}), "signed narrowing above max");
    expect_value(checked_narrow<std::uint32_t>(S{u32max}), u32max, "signed to smaller unsigned max");
    check(!checked_narrow<std::uint32_t>(S{u32max} + S{1}), "signed to smaller unsigned overflow");
    check(!checked_narrow<std::uint32_t>(S{-1}), "signed to smaller unsigned negative");
    expect_value(checked_narrow<S>(u32max), S{u32max}, "unsigned widening into signed");
    expect_value(checked_narrow<S>(s32min), S{s32min}, "signed widening min");
    expect_value(checked_narrow<U>(u32max), U{u32max}, "unsigned widening max");
    check(!checked_narrow<U>(s32min), "signed widening into unsigned still rejects negative");

    constexpr auto size_max = std::numeric_limits<std::size_t>::max();
    expect_value(checked_add(size_max, std::size_t{0}), size_max, "length exact size max");
    check(!checked_add(size_max, std::size_t{1}), "length overflow");
    check(!checked_mul(size_max, std::size_t{2}), "capacity byte overflow");
}

void caller_and_ownership() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    expect_value(layout_offset(3, 8, 10), std::uint32_t{34}, "layout caller success");
    const auto mul_failure = layout_offset(maximum, 2, 0);
    check(!mul_failure && mul_failure.error().context == "layout bytes", "caller stops at multiplication");
    const auto add_failure = layout_offset(1, 1, maximum);
    check(!add_failure && add_failure.error().context == "layout end", "caller stops at offset addition");
    const auto narrow_failure = layout_offset(1, 1, std::numeric_limits<std::uint32_t>::max());
    check(!narrow_failure && narrow_failure.error().context == "layout field", "caller stops at field conversion");
    expect_value(layout_offset(0, maximum, 9), std::uint32_t{9}, "zero count ignores large width safely");

    // 局部 string 含 NUL 且随后被改写，用于证明失败 Error 拥有完整 context 副本。
    const auto owned_error = [maximum] {
        std::string context{"input\0offset", 12};
        const auto result = checked_add(maximum, std::uint64_t{1}, context);
        context.assign("changed");
        return result.error();
    }();
    check(owned_error.context == std::string{"input\0offset", 12} &&
              owned_error.message == "integer addition overflow", "error owns context including embedded NUL");
    const auto mul = checked_mul(maximum, std::uint64_t{2});
    check(!mul && mul.error().context == "checked_mul" &&
              mul.error().message == "integer multiplication overflow", "default multiplication error");
    const auto narrow = checked_narrow<unsigned int>(-1);
    check(!narrow && narrow.error().context == "checked_narrow" &&
              narrow.error().message == "integer conversion out of range", "default conversion error");
    const auto empty = checked_add(maximum, std::uint64_t{1}, {});
    check(!empty && empty.error().context.empty(), "empty context does not mean success");
}

}  // namespace

int main() {
    wide_boundaries();
    caller_and_ownership();
    exhaustive_arithmetic<std::int8_t>();
    exhaustive_arithmetic<std::uint8_t>();
    exhaustive_conversion<std::int8_t, std::int8_t>();
    exhaustive_conversion<std::int8_t, std::uint8_t>();
    exhaustive_conversion<std::uint8_t, std::int8_t>();
    exhaustive_conversion<std::uint8_t, std::uint8_t>();
    std::cout << checks << " behavior checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
