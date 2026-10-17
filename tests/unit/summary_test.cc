/*
 * PROJECT : SIFTWING
 * FILE    : summary_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 以手写原字节/标量窗口验证首命中、预算、完整编码与拥有结果
 */
#include "../support/summary_queries.hpp"
#include "siftwing/query/summary.hpp"
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) {
        throw std::runtime_error(label);
    }
}
const query::SummaryLimits plenty{10000, 100, 10000, 1000000, 1000};
void sample(std::string_view words,
    std::string_view body,
    query::SummaryOptions options,
    std::string_view expected,
    std::uint64_t offset,
    bool matched,
    bool before,
    bool after) {
    const auto input = test::summary_query(words);
    const auto out = query::make_summary(input, body, options, plenty).value();
    check(out.text == expected, "handwritten original text");
    check(out.source_byte_offset == offset, "handwritten byte offset");
    check(out.matched == matched && out.truncated_before == before && out.truncated_after == after,
        "handwritten match and clipping metadata");
    check(
        body.substr(static_cast<std::size_t>(out.source_byte_offset), out.text.size()) == out.text,
        "exact original contiguous bytes");
}
void error(const base::Result<query::Summary> &r, const char *context) {
    check(!r && r.error().context == context, context);
}
} // namespace
int main() {
    try {
        sample("red", "0123Red tail", {5, 1}, "3Red ", 3, true, true, true);
        sample("red|blue", "zz BLUE aa RED", {4, 0}, "BLUE", 3, true, true, true);
        sample("red", "012345red", {5, 1}, "45red", 4, true, true, false);
        sample("red", "中国😀RED尾巴", {5, 1}, "😀RED尾", 6, true, true, true);
        sample("中国", "头中国尾", {2, 0}, "中国", 3, true, true, true);
        sample("中国", "头中国尾", {1, 0}, "中", 3, true, true, true);
        sample("z", "ABC中国😀尾巴", {3, 1}, "ABC", 0, false, false, true);
        sample("x", "", {3, 1}, "", 0, false, false, false);
        sample("x", " \t\n ", {3, 1}, " \t\n", 0, false, false, true);
        sample("he", "the", {2, 0}, "he", 1, true, true, false);
        sample("aa|aaa", "zaaaa", {3, 0}, "aaa", 1, true, true, true);
        sample("abc", "ABC", {20, 10}, "ABC", 0, true, false, false);
        sample("é", "é", {2, 0}, "é", 0, false, false, false);
        sample("é", "Éé", {1, 0}, "é", 2, true, true, false);
        sample("😀", "中😀尾", {1, 0}, "😀", 3, true, true, true);
        sample("x", "👨‍👩‍👧", {1, 0}, "👨", 0, false, false, true);
        sample("x", "\xef\xbb\xbfX", {1, 0}, "X", 3, true, true, false);
        const auto q = test::summary_query("a|bc|a");
        const auto max = std::numeric_limits<std::uint64_t>::max();
        auto limits = plenty;
        // 五正预算独立检查；极小窗口和空正文也不得跳过配置/完整查询准入。
        for (unsigned i = 0; i < 5; ++i) {
            limits = plenty;
            switch (i) {
                case 0:
                    limits.max_content_bytes = 0;
                    break;
                case 1:
                    limits.max_query_terms = 0;
                    break;
                case 2:
                    limits.max_query_bytes = 0;
                    break;
                case 3:
                    limits.max_match_pairs = 0;
                    break;
                default:
                    limits.max_window_scalars = 0;
                    break;
            }
            error(query::make_summary(q, "", {1, 0}, limits), "summary.limits");
        }
        for (auto options : {query::SummaryOptions{0, 0},
                 query::SummaryOptions{1, 1},
                 query::SummaryOptions{1001, 0},
                 query::SummaryOptions{1, max}}) {
            error(query::make_summary(q, "", options, plenty), "summary.options");
        }
        limits = plenty;
        limits.max_content_bytes = 2;
        error(query::make_summary(q, "中", {1, 0}, limits), "summary.content_bytes");
        limits.max_content_bytes = 3;
        check(query::make_summary(q, "中", {1, 0}, limits).value().text == "中",
            "content byte endpoint");
        limits = plenty;
        limits.max_query_terms = 1;
        error(query::make_summary(q, "", {1, 0}, limits), "summary.query_terms");
        limits.max_query_terms = 2;
        limits.max_query_bytes = 2;
        error(query::make_summary(q, "", {1, 0}, limits), "summary.query_bytes");
        limits.max_query_bytes = 3;
        limits.max_match_pairs = 6;
        check(query::make_summary(q, "ab", {1, 0}, limits).value().text == "a",
            "N*Q exact endpoint unique keys");
        limits.max_match_pairs = 5;
        error(query::make_summary(q, "ab", {1, 0}, limits), "summary.work");
        error(query::make_summary(q, "zz", {1, 0}, limits), "summary.work");
        check(query::make_summary(q, "", {1, 0}, limits).value().text.empty(),
            "empty body zero work");
        // 非法尾部不能被早期命中或短窗口掩盖，覆盖过长编码、代理、越界、截断、孤立续字节。
        for (const auto &bad : std::vector<std::string>{std::string("\xc0\xaf", 2),
                 std::string("\xed\xa0\x80", 3),
                 std::string("\xf4\x90\x80\x80", 4),
                 std::string("\xf0\x9f\x98", 3),
                 std::string("\x80", 1),
                 std::string("\xff", 1),
                 std::string("\xe2"
                             "A",
                     2)}) {
            auto r = query::make_summary(q, "a" + bad, {1, 0}, plenty);
            error(r, "summary.utf8");
            check(r.error().message.find("1") != std::string::npos,
                "invalid byte location after prefix");
        }
        const auto nul = query::make_summary(q, std::string("a\0z", 3), {1, 0}, plenty);
        error(nul, "summary.nul");
        check(nul.error().message.find("1") != std::string::npos, "NUL byte location");
        error(query::make_summary(q, std::string("\0\xff", 2), {1, 0}, plenty), "summary.utf8");
        error(query::make_summary(q, std::string("\xff", 1), {0, 0}, plenty), "summary.options");
        limits = plenty;
        limits.max_content_bytes = 1;
        error(query::make_summary(q, std::string("a\xff", 2), {1, 0}, limits),
            "summary.content_bytes");
        check(query::make_summary(q, "\xf4\x8f\xbf\xbf", {1, 0}, plenty).value().text.size() == 4,
            "largest Unicode scalar accepted");
        auto moved = test::summary_query("a");
        const auto owner = std::move(moved);
        error(query::make_summary(moved, "", {1, 0}, plenty), "summary.query");
        check(query::make_summary(owner, "A", {1, 0}, plenty).value().matched,
            "moved destination valid");
        for (auto mode : {query::QueryMode::all, query::QueryMode::any}) {
            const auto input = test::summary_query("a|bc|a", 100, mode);
            check(query::make_summary(input, "bc a", {2, 0}, plenty).value().text == "bc",
                "mode k repetitions do not change earliest text");
        }
        auto owned = [] {
            const auto input = test::summary_query("red");
            const std::string body = "中RED尾";
            return query::make_summary(input, body, {3, 0}, plenty).value();
        }();
        auto copy = owned;
        copy.text.clear();
        check(owned.text == "RED" && owned.source_byte_offset == 3 && copy.text.empty(),
            "owned copy survives all inputs");
        const auto huge =
            query::make_summary(q, "abc", {max, max - 1}, {max, max, max, max, max}).value();
        check(huge.text == "abc" && huge.matched && !huge.truncated_after,
            "uint64 options without overflow allocation");
        check(q.frequencies().terms.at("a") == 2 && q.frequencies().total_tokens == 3,
            "query unchanged on failures");
        std::cout << "summary unit checks=" << checks << " failures=0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
