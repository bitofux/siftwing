/*
 * PROJECT : SIFTWING
 * FILE    : result.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-06
 * BRIEF   : 此模块负责：
 * -- 用显式 success/failure 分支表达进程内操作结果
 * -- 保存成功载荷或拥有型错误信息，并提供符合值类别的访问方式
 * -- 不定义业务错误码、日志策略或异常捕获边界
 */

#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace siftwing::base {

/**
 * @brief 失败分支持有的诊断信息
 *
 * 两个字段均拥有各自字符串；创建 Error 时使用的临时输入销毁后，文本仍然有效。
 * 空文本合法，结果属于 success 还是 failure 只由 Result 的 variant 分支决定。
 *
 * @note 线程安全：不同对象可独立访问；同一对象存在写入时需要调用者同步。
 */
struct Error final {
    /** 面向调用者的失败原因；拥有字符串存储，可为空。 */
    std::string message;

    /** 产生失败的操作上下文；拥有字符串存储，可为空。 */
    std::string context;
};

/**
 * @brief 持有一个成功载荷 T 或一个失败 Error
 *
 * success/failure 工厂显式选择分支；对象不可默认构造，也不可通过赋值替换分支。
 * 活着的对象始终由 storage_ 拥有当前载荷，RAII 销毁随 Result 生命周期发生。
 *
 * @tparam T
 *     非 cv、非数组的对象类型；必须可移动构造且析构为 noexcept。复制能力以及部分
 *     const 右值访问是否可用，继续取决于 T 的实际复制构造能力。
 *
 * @note Result 表达预期失败，不会捕获 T、Error、std::string 或 std::variant 构造时的异常。
 * @note 线程安全：独立对象可并发使用；同一对象的并发读写需要调用者同步。
 */
template <typename T>
class [[nodiscard]] Result final {
    static_assert(std::is_object_v<T> && !std::is_array_v<T> &&
                      !std::is_const_v<T> && !std::is_volatile_v<T>,
                  "Result<T> requires a non-cv, non-array object type");
    static_assert(std::is_move_constructible_v<T>,
                  "Result<T> requires a move-constructible value");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "Result<T> requires a non-throwing destructor");

public:
    /**
     * @brief 复制构造同一结果分支及其载荷
     *
     * @note 仅当底层 variant 可复制构造时可用；载荷复制异常向上传递，源对象保持存活。
     */
    Result(const Result&) = default;

    /**
     * @brief 移动构造同一结果分支及其载荷
     *
     * @note 异常保证取决于载荷类型。成功移动后，源对象仍保持原分支，但其载荷处于该类型
     *     定义的 moved-from 状态，只能执行该类型仍允许的操作。
     */
    Result(Result&&) = default;

    /** @brief 禁止复制赋值，避免在已发布对象上替换结果分支。 */
    Result& operator=(const Result&) = delete;

    /** @brief 禁止移动赋值，避免在已发布对象上替换结果分支。 */
    Result& operator=(Result&&) = delete;

    /**
     * @brief 创建拥有成功载荷的结果
     *
     * @param[in] value
     *     按值取得的载荷；函数将其移动到结果中，成功后由返回对象拥有。
     *
     * @return
     *     success 分支的拥有型 Result。
     *
     * @throws
     *     T 的参数构造或移动构造抛出的异常；异常时不会返回 Result。
     */
    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    /**
     * @brief 创建拥有失败诊断的结果
     *
     * @param[in] error
     *     按值取得的诊断；函数将其移动到 failure 分支，文本由返回对象拥有。
     *
     * @return
     *     failure 分支的拥有型 Result。
     *
     * @throws
     *     Error 或其 std::string 构造、移动过程中产生的异常；异常时不会返回 Result。
     */
    static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    /**
     * @brief 判断当前是否持有成功载荷
     *
     * @retval true
     *     当前分支为 T。
     * @retval false
     *     当前分支为 Error。
     *
     * @note 不访问载荷，且不会改变对象状态。
     */
    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }

    /**
     * @brief 在条件表达式中查询 success/failure 分支
     *
     * @retval true
     *     当前持有成功载荷。
     * @retval false
     *     当前持有失败诊断。
     */
    explicit operator bool() const noexcept { return has_value(); }

    /**
     * @brief 借用可修改的成功载荷
     *
     * @return
     *     由当前 Result 拥有的 T 引用；不延长 Result 生命周期，Result 销毁后失效。
     *
     * @throws std::bad_variant_access
     *     当前为 failure 分支。
     */
    T& value() & { return std::get<0>(storage_); }

    /**
     * @brief 借用只读的成功载荷
     *
     * @return
     *     由当前 Result 拥有的 const T 引用；不延长 Result 生命周期。
     *
     * @throws std::bad_variant_access
     *     当前为 failure 分支。
     */
    const T& value() const & { return std::get<0>(storage_); }

    /**
     * @brief 从非 const 右值结果中取得拥有型成功载荷
     *
     * @return
     *     从内部载荷移动构造的 T 值，不借用临时 Result。
     *
     * @throws std::bad_variant_access
     *     当前为 failure 分支。
     * @throws
     *     T 的移动构造抛出的异常。
     *
     * @post 成功后 Result 仍为 success 分支，内部 T 处于其类型定义的 moved-from 状态。
     */
    T value() && { return std::move(std::get<0>(storage_)); }

    /**
     * @brief 从 const 右值结果中取得拥有型成功载荷副本
     *
     * @return
     *     复制构造的 T 值；不返回指向临时 Result 的引用。
     *
     * @throws std::bad_variant_access
     *     当前为 failure 分支。
     * @throws
     *     T 的复制构造抛出的异常；不可复制的 T 不能使用此重载。
     */
    T value() const && { return std::get<0>(storage_); }

    /**
     * @brief 借用可修改的失败诊断
     *
     * @return
     *     当前 Result 拥有的 Error 引用；不延长 Result 生命周期。
     *
     * @throws std::bad_variant_access
     *     当前为 success 分支。
     */
    Error& error() & { return std::get<1>(storage_); }

    /**
     * @brief 借用只读的失败诊断
     *
     * @return
     *     当前 Result 拥有的 const Error 引用；不延长 Result 生命周期。
     *
     * @throws std::bad_variant_access
     *     当前为 success 分支。
     */
    const Error& error() const & { return std::get<1>(storage_); }

    /**
     * @brief 从非 const 右值结果中取得拥有型失败诊断
     *
     * @return
     *     从内部诊断移动构造的 Error 值。
     *
     * @throws std::bad_variant_access
     *     当前为 success 分支。
     *
     * @post 成功后 Result 仍为 failure 分支，内部 Error 字符串处于有效但内容未指定的状态。
     */
    Error error() && { return std::move(std::get<1>(storage_)); }

    /**
     * @brief 从 const 右值结果中取得拥有型失败诊断副本
     *
     * @return
     *     复制构造的 Error 值，不借用临时 Result。
     *
     * @throws std::bad_variant_access
     *     当前为 success 分支。
     * @throws
     *     Error 中 std::string 复制分配产生的异常。
     */
    Error error() const && { return std::get<1>(storage_); }

private:
    Result(std::in_place_index_t<0>, T&& value)
        : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(std::in_place_index_t<1>, Error&& error)
        : storage_(std::in_place_index<1>, std::move(error)) {}

    /**
     * 按索引区分 T 与 Error，因此 T 本身可以是 Error。该 variant 独占当前载荷；禁止赋值
     * 避免已构造对象切换分支，构造失败则 Result 对象本身不会发布。
     */
    std::variant<T, Error> storage_;
};

/**
 * @brief 不携带成功载荷、只表达成功或失败诊断的 Result 特化
 *
 * success 分支由 std::monostate 表示；failure 分支拥有 Error。对象不可默认构造或赋值。
 *
 * @note 线程安全：独立对象可并发使用；同一对象的并发读写需要调用者同步。
 */
template <>
class [[nodiscard]] Result<void> final {
public:
    /** @brief 复制构造当前分支及失败诊断（若有）；字符串复制异常向上传递。 */
    Result(const Result&) = default;

    /** @brief 移动构造当前分支及失败诊断（若有）；源对象保持原分支。 */
    Result(Result&&) = default;

    /** @brief 禁止复制赋值，避免替换已发布的结果分支。 */
    Result& operator=(const Result&) = delete;

    /** @brief 禁止移动赋值，避免替换已发布的结果分支。 */
    Result& operator=(Result&&) = delete;

    /**
     * @brief 创建不携带载荷的成功结果
     *
     * @return
     *     success 分支的 Result<void>。
     */
    static Result success() { return Result(std::in_place_index<0>); }

    /**
     * @brief 创建拥有失败诊断的结果
     *
     * @param[in] error
     *     按值取得并移动到 failure 分支的诊断。
     *
     * @return
     *     failure 分支的 Result<void>。
     *
     * @throws
     *     Error 或其字符串构造、移动过程中产生的异常。
     */
    static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    /**
     * @brief 判断当前是否为无载荷成功分支
     *
     * @retval true
     *     当前为 success 分支。
     * @retval false
     *     当前为 failure 分支。
     */
    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }

    /**
     * @brief 在条件表达式中查询 success/failure 分支
     *
     * @retval true
     *     当前为 success 分支。
     * @retval false
     *     当前为 failure 分支。
     */
    explicit operator bool() const noexcept { return has_value(); }

    /**
     * @brief 验证当前对象处于无载荷成功分支
     *
     * @throws std::bad_variant_access
     *     当前为 failure 分支。
     */
    void value() const { (void)std::get<0>(storage_); }

    /**
     * @brief 借用可修改的失败诊断
     *
     * @return
     *     当前 Result 拥有的 Error 引用；不延长 Result 生命周期。
     *
     * @throws std::bad_variant_access
     *     当前为 success 分支。
     */
    Error& error() & { return std::get<1>(storage_); }

    /**
     * @brief 借用只读的失败诊断
     *
     * @return
     *     当前 Result 拥有的 const Error 引用；不延长 Result 生命周期。
     *
     * @throws std::bad_variant_access
     *     当前为 success 分支。
     */
    const Error& error() const & { return std::get<1>(storage_); }

    /**
     * @brief 从非 const 右值失败结果中取得拥有型诊断
     *
     * @return
     *     从内部诊断移动构造的 Error 值。
     *
     * @throws std::bad_variant_access
     *     当前为 success 分支。
     *
     * @post 成功后仍为 failure 分支，内部 Error 字符串处于有效但内容未指定的状态。
     */
    Error error() && { return std::move(std::get<1>(storage_)); }

    /**
     * @brief 从 const 右值失败结果中取得拥有型诊断副本
     *
     * @return
     *     复制构造的 Error 值。
     *
     * @throws std::bad_variant_access
     *     当前为 success 分支。
     * @throws
     *     Error 中 std::string 复制分配产生的异常。
     */
    Error error() const && { return std::get<1>(storage_); }

private:
    explicit Result(std::in_place_index_t<0>) : storage_(std::in_place_index<0>) {}
    Result(std::in_place_index_t<1>, Error&& error)
        : storage_(std::in_place_index<1>, std::move(error)) {}

    /** success 使用索引 0 的 monostate，failure 使用索引 1 的拥有型 Error。 */
    std::variant<std::monostate, Error> storage_;
};

}  // namespace siftwing::base
