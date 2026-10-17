/*
 * PROJECT : SIFTWING
 * FILE    : highlight_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 手写区间及HTML验证重叠并集、原字节、转义、预算与拥有性
 */
#include "../support/summary_queries.hpp"
#include "siftwing/query/highlight.hpp"
#include <iostream>
#include <limits>
#include <utility>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) {
        throw std::runtime_error(label);
    }
}
const query::HighlightLimits plenty{10000, 100, 10000, 1000000, 1000, 100000};
void sample(std::string_view words,
    std::string_view body,
    std::string_view html,
    const std::vector<query::HighlightSpan> &spans) {
    const auto q = test::summary_query(words);
    const auto out = query::make_highlight(q, body, plenty).value();
    check(out.html == html, "handwritten safe HTML");
    check(out.spans.size() == spans.size(), "handwritten merged span count");
    for (std::size_t i = 0; i < spans.size(); ++i) {
        check(out.spans[i].byte_offset == spans[i].byte_offset
                  && out.spans[i].byte_length == spans[i].byte_length,
            "handwritten original byte span");
    }
}
void error(const base::Result<query::Highlight> &r, const char *context) {
    check(!r && r.error().context == context, context);
}
} // namespace
int main() {
    try {
        sample("red",
            "Red RED red",
            "<mark>Red</mark> <mark>RED</mark> <mark>red</mark>",
            {{0, 3}, {4, 3}, {8, 3}});
        sample("aa|aaa", "zaaaa", "z<mark>aaaa</mark>", {{1, 4}});
        sample("ab|bc", "zabc", "z<mark>abc</mark>", {{1, 3}});
        sample("a|b", "abba", "<mark>abba</mark>", {{0, 4}});
        sample("a|b", "a b", "<mark>a</mark> <mark>b</mark>", {{0, 1}, {2, 1}});
        sample("中国|中国人", "<中国人>&", "&lt;<mark>中国人</mark>&gt;&amp;", {{1, 9}});
        sample("😀", "中😀尾😀", "中<mark>😀</mark>尾<mark>😀</mark>", {{3, 4}, {10, 4}});
        sample("he", "the", "t<mark>he</mark>", {{1, 2}});
        sample("x", "<script>\"'&</script>", "&lt;script&gt;&quot;&#39;&amp;&lt;/script&gt;", {});
        sample("script",
            "<script>RED</script>",
            "&lt;<mark>script</mark>&gt;RED&lt;/<mark>script</mark>&gt;",
            {{1, 6}, {13, 6}});
        sample("<|&|\"|'", "<&\"'", "<mark>&lt;&amp;&quot;&#39;</mark>", {{0, 4}});
        sample("amp", "&", "&amp;", {}); // 不能在生成实体后匹配，原文没有amp。
        sample("x", "&lt;<mark>", "&amp;lt;&lt;mark&gt;", {}); // 已有实体/标记仍为原文字面值。
        sample("é", "Éé é", "É<mark>é</mark> é", {{2, 2}});
        sample("👩", "👨‍👩‍👧", "👨‍<mark>👩</mark>‍👧", {{7, 4}});
        sample("x", "", "", {});
        sample("x", " \t\n", " \t\n", {});
        sample("x", "\xef\xbb\xbfX", "\xef\xbb\xbf<mark>X</mark>", {{3, 1}});
        const auto q = test::summary_query("a|bc|a");
        for (unsigned i = 0; i < 6; ++i) {
            auto limits = plenty;
            switch (i) {
                case 0:
                    limits.max_text_bytes = 0;
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
                case 4:
                    limits.max_spans = 0;
                    break;
                default:
                    limits.max_output_bytes = 0;
                    break;
            }
            error(query::make_highlight(q, "", limits), "highlight.limits");
        }
        auto limits = plenty;
        limits.max_text_bytes = 2;
        error(query::make_highlight(q, "中", limits), "highlight.text_bytes");
        limits.max_text_bytes = 3;
        check(
            query::make_highlight(q, "中", limits).value().html == "中", "original byte endpoint");
        limits = plenty;
        limits.max_query_terms = 1;
        error(query::make_highlight(q, "", limits), "highlight.query_terms");
        limits.max_query_terms = 2;
        limits.max_query_bytes = 2;
        error(query::make_highlight(q, "", limits), "highlight.query_bytes");
        limits.max_query_bytes = 3;
        limits.max_match_pairs = 6;
        check(query::make_highlight(q, "ab", limits).value().html == "<mark>a</mark>b",
            "N*Q endpoint uses unique keys");
        limits.max_match_pairs = 5;
        error(query::make_highlight(q, "ab", limits), "highlight.work");
        error(query::make_highlight(q, "zz", limits), "highlight.work");
        check(query::make_highlight(q, "", limits).value().html.empty(), "empty text zero work");
        const auto one = test::summary_query("a");
        limits = plenty;
        limits.max_spans = 1;
        error(query::make_highlight(one, "a a", limits), "highlight.spans");
        check(query::make_highlight(one, "aaaa", limits).value().spans[0].byte_length == 4,
            "limit counts merged spans rather than raw hits");
        limits.max_spans = 2;
        check(query::make_highlight(one, "a a", limits).value().spans.size() == 2, "span endpoint");
        // 最终输出预算包含实体及标签，任何普通失败没有半段HTML。
        for (const auto &entry : std::vector<std::pair<std::string, std::uint64_t>>{
                 {"a", 14}, {"&", 5}, {"'", 5}, {"\"", 6}, {"<", 4}, {">", 4}}) {
            limits = plenty;
            limits.max_output_bytes = entry.second;
            check(
                query::make_highlight(one, entry.first, limits).value().html.size() == entry.second,
                "HTML output endpoint");
            --limits.max_output_bytes;
            error(query::make_highlight(one, entry.first, limits), "highlight.output_bytes");
        }
        const auto special = test::summary_query("&");
        limits = plenty;
        limits.max_output_bytes = 18;
        check(query::make_highlight(special, "&", limits).value().html == "<mark>&amp;</mark>",
            "matched entity plus tag endpoint");
        limits.max_output_bytes = 17;
        error(query::make_highlight(special, "&", limits), "highlight.output_bytes");
        // 早期命中、无命中和输出超限都不能掩盖完整非法尾部，UTF-8优先于NUL。
        for (const auto &bad : std::vector<std::string>{std::string("\xc0\xaf", 2),
                 std::string("\xed\xa0\x80", 3),
                 std::string("\xf4\x90\x80\x80", 4),
                 std::string("\xf0\x9f\x98", 3),
                 std::string("\x80", 1),
                 std::string("\xff", 1),
                 std::string("\xe2"
                             "A",
                     2)}) {
            for (const auto &prefix : std::vector<std::string>{"a", "中国"}) {
                limits = plenty;
                limits.max_output_bytes = 1;
                const auto r = query::make_highlight(one, prefix + bad, limits);
                error(r, "highlight.utf8");
                check(r.error().message.find(std::to_string(prefix.size())) != std::string::npos,
                    "invalid original byte offset");
            }
        }
        const auto nul = query::make_highlight(one, std::string("a\0z", 3), plenty);
        error(nul, "highlight.nul");
        check(nul.error().message.find("1") != std::string::npos, "NUL byte offset");
        error(query::make_highlight(one, std::string("\0\xff", 2), plenty), "highlight.utf8");
        limits = plenty;
        limits.max_text_bytes = 1;
        error(query::make_highlight(one, std::string("a\xff", 2), limits), "highlight.text_bytes");
        limits = plenty;
        limits.max_spans = 1;
        limits.max_output_bytes = 1;
        error(query::make_highlight(one, "a a", limits), "highlight.spans");
        check(query::make_highlight(one, "\xf4\x8f\xbf\xbf", plenty).value().html.size() == 4,
            "largest scalar accepted");
        auto moved = test::summary_query("a");
        const auto owner = std::move(moved);
        error(query::make_highlight(moved, "", plenty), "highlight.query");
        check(query::make_highlight(owner, "A", plenty).value().html == "<mark>A</mark>",
            "moved destination remains valid");
        for (auto mode : {query::QueryMode::all, query::QueryMode::any}) {
            const auto input = test::summary_query("a|bc|a", 100, mode);
            check(query::make_highlight(input, "bc a", plenty).value().html
                      == "<mark>bc</mark> <mark>a</mark>",
                "mode k and repetitions do not alter display matching");
        }
        auto owned = [] {
            const auto input = test::summary_query("中国");
            const std::string body = "<中国>";
            return query::make_highlight(input, body, plenty).value();
        }();
        auto copy = owned;
        copy.html.clear();
        copy.spans.clear();
        check(owned.html == "&lt;<mark>中国</mark>&gt;" && owned.spans[0].byte_offset == 1
                  && copy.spans.empty(),
            "output owns all data after input destruction and independent copy modification");
        const auto max = std::numeric_limits<std::uint64_t>::max();
        check(query::make_highlight(one, "A", {max, max, max, max, max, max}).value().html
                  == "<mark>A</mark>",
            "huge budgets do not preallocate to their limits");
        check(q.frequencies().terms.at("a") == 2 && q.frequencies().total_tokens == 3,
            "query unchanged on failure");
        std::cout << "highlight unit checks=" << checks << " failures=0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
