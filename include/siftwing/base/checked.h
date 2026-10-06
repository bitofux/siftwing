/*
 * PROJECT : SIFTWING
 * FILE    : checked.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-06
 * BRIEF   : 此模块负责：
 * -- 为非 bool 整数提供受检加法、乘法和整数值域转换
 * -- 在执行可能产生未定义行为或回绕的表达式前证明结果可表示
 * -- 用 Result 返回拥有调用上下文的失败诊断，不判断业务容量是否合法
 */

#pragma once

#include "siftwing/base/result.h"

#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace siftwing::base {

namespace detail {

/** 仅允许无 cv 修饰、非 bool 的内建或扩展整数类型进入受检模板。 */
template <typename T>
inline constexpr bool checked_integer_v =
    std::is_integral_v<T> && !std::is_same_v<T, bool> &&
    !std::is_const_v<T> && !std::is_volatile_v<T>;

}  // namespace detail

/**
 * @brief 计算两个同类型整数的数学和，仅在 T 可表示时返回成功
 *
 * 检查表达式使用减法边界，避免为了检测溢出而先执行可能回绕或产生未定义行为的加法。
 *
 * @tparam T
 *     无 cv 修饰、非 bool 的整数类型；其他类型在模板实例化时由 static_assert 拒绝。
 * @param[in] lhs
 *     已经转换为 T 的左操作数。
 * @param[in] rhs
 *     已经转换为 T 的右操作数。
 * @param[in] context
 *     调用期间借用的诊断上下文；成功路径不保存，失败路径复制到拥有型 Error，可为空且可含 NUL。
 *
 * @return
 *     数学和可由 T 表示时返回 success 及精确结果；否则返回 failure，message 固定为
 *     "integer addition overflow"，context 为输入副本。
 *
 * @throws
 *     失败路径复制 context 或构造 Result/Error 时产生的分配、构造异常。本函数不以异常表达
 *     整数越界，但没有 noexcept 承诺。
 *
 * @note 参数在进入函数前已经完成的隐式转换或截断无法追回；调用者需要用 checked_narrow
 *     保护跨类型输入。
 * @note 这里只验证整数值域，不验证长度、容量、偏移等业务上限。
 * @note 线程安全：函数无共享可变状态；context 指向的存储在调用期间不得被并发修改或失效。
 */
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

/**
 * @brief 计算两个同类型整数的数学积，仅在 T 可表示时返回成功
 *
 * 零操作数先返回零；其余分支只使用可安全执行的除法比较，特别避免求 abs(min) 或计算
 * min/-1。这样检测过程本身不会触发有符号整数未定义行为。
 *
 * @tparam T
 *     无 cv 修饰、非 bool 的整数类型；其他类型在模板实例化时由 static_assert 拒绝。
 * @param[in] lhs
 *     已经转换为 T 的左操作数。
 * @param[in] rhs
 *     已经转换为 T 的右操作数。
 * @param[in] context
 *     调用期间借用的诊断上下文；成功路径不保存，失败路径复制到拥有型 Error。
 *
 * @return
 *     数学积可由 T 表示时返回 success 及精确结果；否则返回 failure，message 固定为
 *     "integer multiplication overflow"，context 为输入副本。
 *
 * @throws
 *     失败路径复制 context 或构造 Result/Error 时产生的分配、构造异常；没有 noexcept 承诺。
 *
 * @note 任一操作数为零时结果为零，包括另一操作数为有符号最小值的情况。
 * @note 调用前的隐式转换不在检查范围内；整数可表示性也不等于业务容量有效。
 * @note 线程安全：函数无共享可变状态；context 指向的存储在调用期间不得被并发修改或失效。
 */
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
            // 两负数用 maximum/rhs 建立上界，检查本身不计算 min/-1 或 abs(min)。
            overflow = rhs > 0 ? lhs < minimum / rhs : lhs < maximum / rhs;
        }
    }
    if (overflow) {
        return Result<T>::failure({"integer multiplication overflow", std::string{context}});
    }
    return Result<T>::success(static_cast<T>(lhs * rhs));
}

/**
 * @brief 在整数值能够由目标类型完整表示时执行转换
 *
 * 同宽和扩宽转换也按 signedness 检查实际值域。实现仅在目标边界可由 From 表示的编译期
 * 分支中比较边界，证明安全后才执行 static_cast<To>(value)。
 *
 * @tparam To
 *     目标整数类型，必须无 cv 修饰且不是 bool。
 * @tparam From
 *     源整数类型，必须无 cv 修饰且不是 bool。
 * @param[in] value
 *     已经是 From 的源值；更早发生的转换或截断不在本函数观察范围内。
 * @param[in] context
 *     调用期间借用的诊断上下文；成功路径不保存，失败路径复制到拥有型 Error。
 *
 * @return
 *     value 位于 To 的值域时返回 success 及精确转换值；否则返回 failure，message 固定为
 *     "integer conversion out of range"，context 为输入副本。
 *
 * @throws
 *     失败路径复制 context 或构造 Result/Error 时产生的分配、构造异常；没有 noexcept 承诺。
 *
 * @note std::numeric_limits<T>::digits 表示不含符号位的有效位数；实现结合 signedness 决定
 *     哪些边界比较在 From 中可表示。
 * @note 本函数只证明整数值域转换无损，不证明转换后的值满足业务容量、索引或协议约束。
 * @note 线程安全：函数无共享可变状态；context 指向的存储在调用期间不得被并发修改或失效。
 */
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
