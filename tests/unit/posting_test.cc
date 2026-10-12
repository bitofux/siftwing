/*
 * PROJECT : SIFTWING
 * FILE    : posting_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 验证原始TF/身份值域、完整值语义及列表局部结构，不使用assert隐藏Release行为
 */
#include "siftwing/index/posting.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, std::string_view description) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
auto id(std::uint64_t value) { return siftwing::base::DocumentId::from_integer(value).value(); }
auto posting(std::uint64_t document, std::uint64_t tf = 1) {
    return siftwing::index::Posting::from_count(id(document), tf).value();
}
template <typename T>
void error(const siftwing::base::Result<T>& result, std::string_view context) {
    check(!result, "expected failure");
    if (!result) {
        check(result.error().context == context && !result.error().message.empty(), context);
    }
}
} // namespace

int main() {
    try {
        using namespace siftwing::index;
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        // 强ID完整值域与次数完整值域相互独立，超过double精确整数范围仍逐位保留。
        for (auto document : std::array<std::uint64_t, 3>{0, 7, maximum}) {
            for (auto count : std::array<std::uint64_t, 5>{1, 2, 9007199254740993ULL, maximum-1, maximum}) {
                auto result = Posting::from_count(id(document), count);
                check(static_cast<bool>(result), "full width positive TF accepted");
                if (result) {
                    check(result.value().document_id() == id(document) &&
                          result.value().term_frequency() == count, "exact independent pair");
                }
            }
        }
        for (auto count : std::array<std::int64_t, 4>{std::numeric_limits<std::int64_t>::min(), -7, -1, 0}) {
            error(Posting::from_count(id(0), count), "posting.tf");
        }
        error(Posting::from_count(id(maximum), 0U), "posting.tf");
        check(Posting::from_count(id(0), std::int8_t{127}).value().term_frequency() == 127,
              "signed source checked before conversion");
        check(Posting::from_count(id(0), std::uint8_t{255}).value().term_frequency() == 255,
              "small unsigned source preserved");
        auto original = posting(7, maximum);
        auto copied = original;
        auto moved = std::move(original);
        check(original == copied && moved == copied, "move of scalar pair preserves source");
        auto assigned = posting(42, 1);
        assigned = copied;
        check(assigned == copied, "copy assignment replaces complete pair");
        assigned = std::move(moved);
        check(assigned == copied && moved == copied, "move assignment preserves both complete pairs");
        auto* alias = &assigned;
        assigned = *alias;
        assigned = std::move(*alias);
        check(assigned == copied, "self assignment preserves complete pair");
        check(posting(7, 1) != posting(7, 2) && posting(7, 1) != posting(8, 1),
              "equality includes both fields without ordering TF");
        auto escaped = [] { auto local=posting(42, 3); return local.document_id(); }();
        check(escaped == id(42), "accessor returns independent ID value");

        error(validate_posting_list({}, {}), "posting.limits");
        check(static_cast<bool>(validate_posting_list({}, {1})), "empty valid with positive configuration");
        const PostingList exact{posting(0, maximum), posting(7, maximum), posting(maximum, maximum)};
        check(static_cast<bool>(validate_posting_list(exact, {3})), "inclusive budget and sum not computed");
        error(validate_posting_list(exact, {2}), "posting.size");
        error(validate_posting_list({posting(7), posting(0)}, {0}), "posting.limits");
        error(validate_posting_list({posting(7), posting(0)}, {1}), "posting.size");
        error(validate_posting_list({posting(7, 1), posting(7, maximum)}, {2}), "posting.order");
        auto late = validate_posting_list({posting(0), posting(7), posting(7)}, {3});
        error(late, "posting.order");
        check(!late && late.error().message.find("posting 2") != std::string::npos, "first offending zero based position");

        // 独立参考为排序后的完整序列相等且集合唯一；枚举4^4序列覆盖每个位置的逆序/重复。
        // 每个候选在返回成功或失败后逐项对照原值，证明校验不排序、不合并或写回。
        const std::array<std::uint64_t, 4> domain{0, 1, 7, maximum};
        for (unsigned code=0; code<256; ++code) {
            auto rest=code;
            PostingList values;
            for (unsigned position=0; position<4; ++position) {
                values.push_back(posting(domain[rest%4], position+1));
                rest/=4;
            }
            const auto before = values;
            std::array<std::uint64_t, 4> numbers{};
            for (std::size_t i=0; i<numbers.size(); ++i) { numbers[i]=values[i].document_id().value(); }
            auto sorted = numbers;
            std::sort(sorted.begin(), sorted.end());
            const bool expected = numbers==sorted && std::adjacent_find(sorted.begin(),sorted.end())==sorted.end();
            check(static_cast<bool>(validate_posting_list(values, {4})) == expected, "enumerated independent order/set reference");
            check(values==before, "all paths preserve input pair sequence");
            error(validate_posting_list(values, {3}), "posting.size");
        }
        PostingList mutable_list{posting(0), posting(7)};
        check(static_cast<bool>(validate_posting_list(mutable_list, {2})), "initial valid list");
        mutable_list[1] = posting(0, 3);
        error(validate_posting_list(mutable_list, {2}), "posting.order");
        // 成功只验证当时结构，不冻结可变vector；单项工厂仍确保正TF。
        mutable_list.clear();
        check(static_cast<bool>(validate_posting_list(mutable_list, {1})), "mutation requires new validation");
        check(static_cast<bool>(validate_posting_list({posting(maximum, 1)}, {maximum})), "maximum is ordinary identity and budget");
    } catch (const std::exception& exception) {
        ++failures; std::cerr << "unexpected: " << exception.what() << '\n';
    }
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
