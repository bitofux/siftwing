#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace siftwing::base {

// 拥有错误文本；局部输入销毁后仍可读取。文本内容不决定结果分支。
struct Error final {
    std::string message;
    std::string context;
};

// 成功或失败由工厂明确选择。禁止赋值/替换分支，避免异常后的无载荷状态。
// 左值访问返回借用引用；右值访问返回拥有值，不借用临时结果。
// 误取分支抛 std::bad_variant_access；构造/复制/移动异常向上传递。
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
    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = delete;
    Result& operator=(Result&&) = delete;

    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { return std::get<0>(storage_); }
    const T& value() const & { return std::get<0>(storage_); }
    T value() && { return std::move(std::get<0>(storage_)); }
    T value() const && { return std::get<0>(storage_); }

    Error& error() & { return std::get<1>(storage_); }
    const Error& error() const & { return std::get<1>(storage_); }
    Error error() && { return std::move(std::get<1>(storage_)); }
    Error error() const && { return std::get<1>(storage_); }

private:
    Result(std::in_place_index_t<0>, T&& value)
        : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(std::in_place_index_t<1>, Error&& error)
        : storage_(std::in_place_index<1>, std::move(error)) {}

    // 使用索引选择分支，因此 T 本身也可以是 Error。
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> final {
public:
    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = delete;
    Result& operator=(Result&&) = delete;

    static Result success() { return Result(std::in_place_index<0>); }
    static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    void value() const { (void)std::get<0>(storage_); }

    Error& error() & { return std::get<1>(storage_); }
    const Error& error() const & { return std::get<1>(storage_); }
    Error error() && { return std::move(std::get<1>(storage_)); }
    Error error() const && { return std::get<1>(storage_); }

private:
    explicit Result(std::in_place_index_t<0>) : storage_(std::in_place_index<0>) {}
    Result(std::in_place_index_t<1>, Error&& error)
        : storage_(std::in_place_index<1>, std::move(error)) {}

    std::variant<std::monostate, Error> storage_;
};

}  // namespace siftwing::base
