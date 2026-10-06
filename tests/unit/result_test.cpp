#include "siftwing/base/result.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using siftwing::base::Error;
using siftwing::base::Result;

static_assert(!std::is_default_constructible_v<Result<int>>);
static_assert(!std::is_default_constructible_v<Result<void>>);
static_assert(!std::is_copy_assignable_v<Result<int>>);
static_assert(!std::is_move_assignable_v<Result<int>>);
static_assert(!std::is_copy_assignable_v<Result<void>>);
static_assert(!std::is_move_assignable_v<Result<void>>);
static_assert(std::is_copy_constructible_v<Result<std::string>>);
static_assert(std::is_move_constructible_v<Result<std::unique_ptr<int>>>);
static_assert(!std::is_copy_constructible_v<Result<std::unique_ptr<int>>>);
static_assert(std::is_same_v<decltype(std::declval<Result<int>&>().value()), int&>);
static_assert(std::is_same_v<decltype(std::declval<const Result<int>&>().value()),
                             const int&>);
static_assert(std::is_same_v<decltype(std::declval<Result<std::string>&&>().value()),
                             std::string>);
static_assert(std::is_same_v<decltype(std::declval<const Result<std::string>&&>().value()),
                             std::string>);
static_assert(std::is_same_v<decltype(std::declval<Result<void>&&>().error()), Error>);
static_assert(std::is_same_v<decltype(std::declval<const Result<int>&&>().error()), Error>);

int failures = 0;
int checks = 0;

void check(bool condition, std::string_view label) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

template <typename Operation>
void check_bad_access(Operation operation, std::string_view label) {
    try {
        operation();
        check(false, label);
    } catch (const std::bad_variant_access&) {
        check(true, label);
    } catch (...) {
        check(false, label);
    }
}

// 验收夹具：生产结果再由上游分支处理，不是生产业务入口。
Result<int> positive_value(int input) {
    if (input <= 0) {
        return Result<int>::failure({"value must be positive", "positive_value"});
    }
    return Result<int>::success(input);
}

Result<void> validate_positive(int input) {
    const auto result = positive_value(input);
    if (!result) {
        return Result<void>::failure(result.error());
    }
    return Result<void>::success();
}

Error escaped_error() {
    std::string message = "invalid input";
    std::string context = "local operation";
    auto result = Result<int>::failure({message, context});
    message = "changed";
    context.clear();
    return std::move(result).error();
}

struct Tracked final {
    inline static int live = 0;
    inline static bool throw_copy = false;
    inline static bool throw_move = false;
    int number;

    explicit Tracked(int input) : number(input) { ++live; }
    Tracked(const Tracked& other) : number(other.number) {
        if (throw_copy) {
            throw std::runtime_error("copy refused");
        }
        ++live;
    }
    Tracked(Tracked&& other) : number(other.number) {
        if (throw_move) {
            throw std::runtime_error("move refused");
        }
        ++live;
        other.number = 0;
    }
    ~Tracked() { --live; }
};

void check_construction_failures() {
    {
        auto original = Result<Tracked>::success(Tracked{9});
        check(Tracked::live == 1, "one owned payload after factory temporaries");
        Tracked::throw_copy = true;
        try {
            const auto copy = original;
            (void)copy;
            check(false, "copy construction exception propagates");
        } catch (const std::runtime_error&) {
            check(true, "copy construction exception propagates");
        }
        Tracked::throw_copy = false;
        check(original.has_value() && original.value().number == 9 && Tracked::live == 1,
              "failed copy preserves source branch and resource count");

        Tracked::throw_move = true;
        try {
            auto moved = std::move(original);
            (void)moved;
            check(false, "move construction exception propagates");
        } catch (const std::runtime_error&) {
            check(true, "move construction exception propagates");
        }
        check(original.has_value() && original.value().number == 9 && Tracked::live == 1,
              "failed move retains source branch without leaking payload");
        try {
            auto result = Result<Tracked>::success(Tracked{4});
            (void)result;
            check(false, "factory construction exception propagates");
        } catch (const std::runtime_error&) {
            check(true, "factory construction exception propagates");
        }
        Tracked::throw_move = false;
        check(Tracked::live == 1, "failed factory releases its parameter");
    }
    check(Tracked::live == 0, "all payloads released after scope");
}

}  // namespace

int main() {
    auto success = positive_value(7);
    check(success.has_value() && static_cast<bool>(success) && success.value() == 7,
          "producer to caller success branch");
    success.value() = 8;
    const auto& const_success = success;
    check(const_success.value() == 8, "left value references refer to the owned payload");
    check(Result<int>::success(0).has_value(), "zero value is a success");
    check(Result<bool>::success(false).has_value(), "false payload is a success");

    auto failure = positive_value(-1);
    check(!failure && !failure.has_value(), "producer to caller failure branch");
    check(failure.error().message == "value must be positive" &&
              failure.error().context == "positive_value",
          "error reason and operation context");
    const auto& const_failure = failure;
    check(&const_failure.error() == &failure.error(), "const error borrows the same payload");
    check(!Result<int>::failure({"", ""}).has_value(), "empty error remains a failure");
    const auto error = escaped_error();
    check(error.message == "invalid input" && error.context == "local operation",
          "error text survives original input mutation and destruction");

    auto good_void = validate_positive(3);
    good_void.value();
    check(good_void.has_value() && static_cast<bool>(good_void), "void operation succeeds");
    auto bad_void = validate_positive(0);
    check(!bad_void && bad_void.error().context == "positive_value",
          "void caller propagates the original error context");

    check_bad_access([&success] { (void)success.error(); }, "error on successful value throws");
    check_bad_access([&failure] { (void)failure.value(); }, "value on failure throws");
    check_bad_access([&const_success] { (void)const_success.error(); }, "const wrong error throws");
    check_bad_access([&const_failure] { (void)const_failure.value(); }, "const wrong value throws");
    check_bad_access([&good_void] { (void)good_void.error(); }, "error on void success throws");
    check_bad_access([&bad_void] { bad_void.value(); }, "value on void failure throws");
    check_bad_access([&failure] { (void)std::move(failure).value(); }, "rvalue wrong value throws");
    check_bad_access([&bad_void] { (void)std::move(bad_void).value(); }, "rvalue void failure throws");
    check(success.has_value() && !failure && good_void && !bad_void,
          "wrong accesses do not change branches");

    auto copy = success;
    copy.value() = 42;
    check(success.value() == 8 && copy.value() == 42, "copied values are independent");
    auto copied_failure = failure;
    copied_failure.error().context = "upper layer";
    check(failure.error().context == "positive_value", "copied errors are independent");
    auto copied_void = bad_void;
    check(!copied_void && copied_void.error().message == bad_void.error().message,
          "void errors are copyable");

    auto pointer = std::make_unique<int>(21);
    auto owner = Result<std::unique_ptr<int>>::success(std::move(pointer));
    check(!pointer && owner.has_value() && *owner.value() == 21, "factory takes move-only ownership");
    auto moved_owner = std::move(owner);
    check(owner.has_value() && !owner.value() && *moved_owner.value() == 21,
          "move construction transfers resource and preserves source branch");
    auto extracted = std::move(moved_owner).value();
    check(*extracted == 21 && moved_owner.has_value() && !moved_owner.value(),
          "rvalue value extraction transfers ownership");
    const auto text = Result<std::string>::success("temporary value").value();
    check(text == "temporary value", "temporary result returns an owning value");
    const auto const_text = Result<std::string>::success("const value");
    check(std::move(const_text).value() == "const value" && const_text.value() == "const value",
          "const rvalue extraction copies rather than returning a dangling reference");
    const auto moved_error = std::move(failure).error();
    check(moved_error.context == "positive_value" && !failure, "rvalue error extraction owns text");
    const auto const_error = Result<void>::failure({"reason", "context"});
    check(std::move(const_error).error().context == "context" && !const_error,
          "const void rvalue error extraction owns a copy");

    auto error_value = Result<Error>::success({"data", "payload"});
    auto error_failure = Result<Error>::failure({"reason", "operation"});
    check(error_value.has_value() && error_value.value().context == "payload" &&
              !error_failure && error_failure.error().context == "operation",
          "Error as success payload remains distinct from failure");
    check_construction_failures();
    std::cout << checks << " behavior checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
