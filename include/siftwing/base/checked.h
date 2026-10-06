#pragma once

#include "siftwing/base/result.h"

#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace siftwing::base {

namespace detail {

template <typename T>
inline constexpr bool checked_integer_v =
    std::is_integral_v<T> && !std::is_same_v<T, bool> &&
    !std::is_const_v<T> && !std::is_volatile_v<T>;

}  // namespace detail

// 输入已经是T；调用前的隐式转换不在检查范围内。失败拥有context副本。
template <typename T>
Result<T> checked_add(T lhs, T rhs, std::string_view context = "checked_add") {
    static_assert(detail::checked_integer_v<T>,
                  "checked arithmetic requires non-cv, non-bool integral types");
    constexpr T maximum = std::numeric_limits<T>::max();
    bool overflow;
    if constexpr (std::is_unsigned_v<T>) {
        overflow = lhs > maximum - rhs;
    } else {
        constexpr T minimum = std::numeric_limits<T>::min();
        overflow = (rhs > 0 && lhs > maximum - rhs) ||
                   (rhs < 0 && lhs < minimum - rhs);
    }
    if (overflow) {
        return Result<T>::failure({"integer addition overflow", std::string{context}});
    }
    return Result<T>::success(static_cast<T>(lhs + rhs));
}

template <typename T>
Result<T> checked_mul(T lhs, T rhs, std::string_view context = "checked_mul") {
    static_assert(detail::checked_integer_v<T>,
                  "checked arithmetic requires non-cv, non-bool integral types");
    if (lhs == 0 || rhs == 0) {
        return Result<T>::success(T{0});
    }
    constexpr T maximum = std::numeric_limits<T>::max();
    bool overflow;
    if constexpr (std::is_unsigned_v<T>) {
        overflow = lhs > maximum / rhs;
    } else {
        constexpr T minimum = std::numeric_limits<T>::min();
        if (lhs > 0) {
            overflow = rhs > 0 ? lhs > maximum / rhs : rhs < minimum / lhs;
        } else {
            // 两负数用max/rhs，不能在检查中计算min/-1或abs(min)。
            overflow = rhs > 0 ? lhs < minimum / rhs : lhs < maximum / rhs;
        }
    }
    if (overflow) {
        return Result<T>::failure({"integer multiplication overflow", std::string{context}});
    }
    return Result<T>::success(static_cast<T>(lhs * rhs));
}

// 同宽/扩宽也检查值域；先证明目标界限可表示为From，最后才转换value。
template <typename To, typename From>
Result<To> checked_narrow(From value, std::string_view context = "checked_narrow") {
    static_assert(detail::checked_integer_v<To> && detail::checked_integer_v<From>,
                  "checked conversion requires non-cv, non-bool integral types");
    constexpr int to_digits = std::numeric_limits<To>::digits;
    constexpr int from_digits = std::numeric_limits<From>::digits;
    bool outside = false;
    if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
        if constexpr (to_digits < from_digits) {
            outside = value < static_cast<From>(std::numeric_limits<To>::min()) ||
                      value > static_cast<From>(std::numeric_limits<To>::max());
        }
    } else if constexpr (std::is_signed_v<From>) {
        outside = value < 0;
        if constexpr (to_digits < from_digits) {
            outside = outside || value > static_cast<From>(std::numeric_limits<To>::max());
        }
    } else {
        if constexpr (to_digits < from_digits) {
            outside = value > static_cast<From>(std::numeric_limits<To>::max());
        }
    }
    if (outside) {
        return Result<To>::failure({"integer conversion out of range", std::string{context}});
    }
    return Result<To>::success(static_cast<To>(value));
}

}  // namespace siftwing::base
