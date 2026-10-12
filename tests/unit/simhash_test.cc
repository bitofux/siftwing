/*
 * PROJECT : SIFTWING
 * FILE    : simhash_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 用固定词哈希、手算多数关系和独立逐位距离验证指纹合同
 */
#include "siftwing/document/simhash.hpp"

#include <array>
#include <iostream>
#include <limits>

namespace {
using namespace siftwing::document;
using siftwing::text::DocumentTermFrequencies;
unsigned checks = 0, failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
constexpr auto max = std::numeric_limits<std::uint64_t>::max();
const SimHashLimits broad{100, 100, 4096, max};
void expect(const DocumentTermFrequencies &tf, std::uint64_t bits, const char *label) {
    const auto result = make_simhash(tf, broad);
    check(result && result.value() && result.value()->bits == bits, label);
}
void error(const DocumentTermFrequencies &tf, const SimHashLimits &limits, const char *context) {
    const auto result = make_simhash(tf, limits);
    check(!result, "invalid TF or budget rejected");
    if (!result) {
        check(result.error().context == context, "accurate fingerprint failure context");
        bool absent = false;
        try {
            (void)result.value();
        } catch (const std::bad_variant_access &) {
            absent = true;
        }
        check(absent, "failure has no optional or partial fingerprint");
    }
}
} // namespace

int main() {
    try {
        static_assert(noexcept(hamming_distance({}, {})));
        check(simhash_policy_version == 1, "fixed policy version");
        // 固定值由独立任意精度字节参考计算并冻结；投票关系手算，不向生产实现取预期。
        constexpr std::uint64_t a = 0x82a2a958a9bece5bULL, b = 0x6e673288764ad2d0ULL;
        expect({{{"a", 1}}, 1}, a, "single ASCII feature fixed hash");
        expect({{{"b", 1}}, 1}, b, "second ASCII feature fixed hash");
        expect({{{"中国", 1}}, 1}, 0xb76ab2bc7f31fcf1ULL, "UTF8 bytes are unsigned");
        expect({{{"😀", 1}}, 1}, 0xda89b0991f06f386ULL, "four-byte scalar fixed hash");
        expect({{{"a", 1}, {"b", 1}}, 2}, a & b, "equal votes tie to zero is bitwise AND");
        expect({{{"a", 3}, {"b", 2}}, 5}, a, "strict majority feature controls every bit");
        expect({{{"a", 300}, {"b", 200}}, 500}, a, "positive scaling leaves direction unchanged");
        expect({{{"a", max}}, max}, a, "full uint64 weight without signed conversion");
        expect({{{"a", (max / 2) + 1}, {"b", max / 2}}, max}, a,
               "one vote majority across full uint64 range");
        expect({{{"a", max / 2}, {"b", max / 2}}, max - 1}, a & b,
               "large exact ties do not round through double");
        auto empty = make_simhash({}, broad);
        check(empty && !empty.value(), "empty feature table is successful absence");
        // 结构验证、完整编码优先于格式验证，非空零指纹仍由类型合法表达。
        check(SimHash64{0}.bits == 0, "zero is a valid fingerprint bit pattern");
        error({{}, 1}, broad, "simhash.table");
        error({{{"a", 0}}, 0}, broad, "simhash.table");
        error({{{"a", 1}}, 2}, broad, "simhash.table");
        error({{{"a", max}, {"b", 1}}, max}, broad, "simhash.overflow");
        error({{{"", 1}}, 1}, broad, "simhash.term");
        for (const std::string word : {"a b", "a\tb", "a\nb", "a　b", "a\xc2\x85"}) {
            error({{{word, 1}}, 1}, broad, "simhash.term");
        }
        const std::array<const char *, 25> spaces{
            "\t", "\n", "\v", "\f", "\r", " ", "\u0085", "\u00a0", "\u1680",
            "\u2000", "\u2001", "\u2002", "\u2003", "\u2004", "\u2005", "\u2006",
            "\u2007", "\u2008", "\u2009", "\u200a", "\u2028", "\u2029", "\u202f",
            "\u205f", "\u3000"};
        for (const auto *space : spaces) {
            error({{{std::string("a") + space + "b", 1}}, 1}, broad, "simhash.term");
        }
        check(static_cast<bool>(make_simhash({{{"\ufeff",1},{"\u200b",1}},2}, broad)),
              "BOM scalar and zero-width space are literal non-White_Space keys");
        const std::array<std::string, 10> invalid{
            "\x80", "\xc0\xaf", "\xe2\x82", "\xed\xa0\x80", "\xf4\x90\x80\x80",
            "\xf5\x80\x80\x80", "\xff", "\xe0\x80\xaf", "\xc2x", "\xf0\x80\x80\xaf"};
        for (const auto &word : invalid) {
            error({{{word, 1}}, 1}, broad, "simhash.utf8");
        }
        error({{{std::string("a\0b", 3), 1}}, 1}, broad, "simhash.nul");
        error({{{"", 0}, {"z\xff", 1}}, 1}, broad, "simhash.utf8");
        auto diagnostic = make_simhash({{{"a\xff", 1}}, 1}, broad);
        check(!diagnostic && diagnostic.error().message.find("term 0") != std::string::npos &&
                  diagnostic.error().message.find("byte 1") != std::string::npos,
              "encoding diagnosis locates term and byte");
        const DocumentTermFrequencies two{{{"aa", 2}, {"bb", 1}}, 3};
        const SimHashLimits exact{2, 2, 4, 3};
        check(static_cast<bool>(make_simhash(two, exact)), "all exact budgets accepted");
        const std::array<std::uint64_t SimHashLimits::*, 4> members{
            &SimHashLimits::max_distinct_terms, &SimHashLimits::max_term_bytes,
            &SimHashLimits::max_vocabulary_bytes, &SimHashLimits::max_total_tokens};
        const std::array<const char *, 4> contexts{
            "simhash.distinct_terms", "simhash.term_bytes", "simhash.vocabulary_bytes",
            "simhash.total_tokens"};
        for (std::size_t i = 0; i < members.size(); ++i) {
            auto limits = exact;
            --(limits.*members[i]);
            error(two, limits, contexts[i]);
            limits.*members[i] = 0;
            error({}, limits, "simhash.limits");
        }
        // 独立逐位加总穷举8位值对；另逐一验证全64位，防止截成32位或错误阈值短路。
        for (std::uint64_t left = 0; left < 256; ++left) {
            for (std::uint64_t right = 0; right < 256; ++right) {
                std::uint32_t count = 0;
                for (unsigned bit = 0; bit < 8; ++bit) {
                    count += static_cast<std::uint32_t>(((left >> bit) & 1U) != ((right >> bit) & 1U));
                }
                check(hamming_distance({left}, {right}) == count, "exhaustive independent 8bit distance");
            }
        }
        for (unsigned bit = 0; bit < 64; ++bit) {
            check(hamming_distance({0}, {std::uint64_t{1} << bit}) == 1, "every individual 64bit position");
        }
        check(hamming_distance({0}, {max}) == 64, "all 64 bits differ");
        check(hamming_distance({a}, {a}) == 0, "same fingerprint distance zero");
        check(hamming_distance({a}, {b}) == hamming_distance({b}, {a}), "distance symmetry");
        auto saved = make_simhash(two, exact);
        auto input = two;
        auto owned = make_simhash(input, broad);
        input.terms.clear();
        input.total_tokens = 0;
        check(saved && owned && saved.value()->bits == owned.value()->bits && two.terms.size() == 2,
              "result owns bits beyond input mutation and original input unchanged");
    } catch (const std::exception &e) {
        ++failures;
        std::cerr << "unexpected: " << e.what() << '\n';
    }
    std::cout << "simhash checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
