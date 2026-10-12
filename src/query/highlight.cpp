/*
 * PROJECT : SIFTWING
 * FILE    : highlight.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 完整准入后在原文定位、合并区间并预检转义HTML大小
 * IMPLEMENTATION : 匹配在原标量层完成，边界映回原字节；实体只在最终渲染时生成。
 */
#include "siftwing/query/highlight.h"
#include "siftwing/base/checked.h"
#include <utf8.h>
#include <algorithm>
#include <utility>

namespace siftwing::query {
namespace {
using Points = std::vector<std::uint32_t>;
base::Result<Highlight> fail(const char *context, std::string message) {
    return base::Result<Highlight>::failure({std::move(message), context});
}
std::uint32_t fold_ascii(std::uint32_t point) noexcept {
    return point >= 'A' && point <= 'Z' ? point + ('a' - 'A') : point;
}
std::uint64_t scalar_count(std::string_view bytes) {
    std::uint64_t count = 0;
    auto it = bytes.begin();
    while (it != bytes.end()) { (void)utf8::next(it, bytes.end()); ++count; }
    return count;
}
// 只替换HTML文本的五个ASCII特殊字节；合法UTF-8多字节内容按原值复制。
std::string_view entity(char byte) noexcept {
    switch (byte) {
    case '&': return "&amp;";
    case '<': return "&lt;";
    case '>': return "&gt;";
    case '"': return "&quot;";
    case '\'': return "&#39;";
    default: return {};
    }
}
} // namespace

base::Result<Highlight> make_highlight(const Query &query, std::string_view text,
                                      const HighlightLimits &limits) {
    if (limits.max_text_bytes == 0 || limits.max_query_terms == 0 ||
        limits.max_query_bytes == 0 || limits.max_match_pairs == 0 ||
        limits.max_spans == 0 || limits.max_output_bytes == 0) {
        return fail("highlight.limits", "高亮需要六项正预算");
    }
    if (query.empty()) { return fail("highlight.query", "移空查询不能生成高亮"); }
    const auto bytes = base::checked_narrow<std::uint64_t>(text.size(), "highlight.text_bytes");
    if (!bytes || bytes.value() > limits.max_text_bytes) {
        return fail("highlight.text_bytes", "完整文字超出字节预算");
    }
    const auto invalid = utf8::find_invalid(text.begin(), text.end());
    if (invalid != text.end()) {
        return fail("highlight.utf8", "非法UTF-8原字节位置 " + std::to_string(invalid - text.begin()));
    }
    const auto nul = text.find('\0');
    if (nul != text.npos) { return fail("highlight.nul", "NUL原字节位置 " + std::to_string(nul)); }
    const auto &terms = query.frequencies().terms;
    const auto count = base::checked_narrow<std::uint64_t>(terms.size(), "highlight.query_terms");
    if (!count || count.value() > limits.max_query_terms) {
        return fail("highlight.query_terms", "完整唯一查询词数超出预算");
    }
    std::uint64_t key_bytes = 0;
    std::uint64_t key_points = 0;
    for (const auto &term : terms) {
        const auto size = base::checked_narrow<std::uint64_t>(term.first.size(), "highlight.query_bytes");
        if (!size || size.value() > limits.max_query_bytes - key_bytes) {
            return fail("highlight.query_bytes", "完整唯一键累计字节超出预算");
        }
        key_bytes += size.value();
        // 有效Query保证非空合法键，Q>0且累计标量不超过已准入键字节。
        key_points += scalar_count(term.first);
    }
    const auto n = scalar_count(text);
    if (n > limits.max_match_pairs / key_points) {
        return fail("highlight.work", "完整文字与全部查询键的标量匹配预算不足");
    }
    Points body;
    std::vector<std::size_t> boundaries{0};
    auto it = text.begin();
    while (it != text.end()) {
        body.push_back(fold_ascii(utf8::next(it, text.end())));
        boundaries.push_back(static_cast<std::size_t>(it - text.begin()));
    }
    std::vector<Points> keys;
    for (const auto &term : terms) {
        Points key;
        auto key_it = term.first.begin();
        while (key_it != term.first.end()) {
            key.push_back(fold_ascii(utf8::next(key_it, term.first.end())));
        }
        keys.push_back(std::move(key));
    }
    Highlight result;
    for (std::size_t i = 0; i < body.size(); ++i) {
        std::size_t end = i;
        for (const auto &key : keys) {
            if (key.size() > body.size() - i) { continue; }
            if (std::equal(key.begin(), key.end(), body.begin() + static_cast<Points::difference_type>(i))) {
                end = std::max(end, i + key.size());
            }
        }
        if (end == i) { continue; }
        const auto start_byte = static_cast<std::uint64_t>(boundaries[i]);
        const auto end_byte = static_cast<std::uint64_t>(boundaries[end]);
        // 起点递增，同起点只保留最长末端；重叠或相邻可在线扩展最后区间，不存全部原始命中。
        if (!result.spans.empty() && start_byte <=
                result.spans.back().byte_offset + result.spans.back().byte_length) {
            auto &last = result.spans.back();
            last.byte_length = std::max(last.byte_length, end_byte - last.byte_offset);
        } else {
            if (result.spans.size() >= limits.max_spans) {
                return fail("highlight.spans", "合并命中区间数超出预算");
            }
            result.spans.push_back({start_byte, end_byte - start_byte});
        }
    }
    // 完整计算实体和标签后的字节数，在分配/发布HTML前拒绝溢出或输出超限。
    std::uint64_t output_bytes = 0;
    for (char byte : text) {
        const auto escaped = entity(byte);
        const auto length = escaped.empty() ? std::uint64_t{1} : static_cast<std::uint64_t>(escaped.size());
        if (length > limits.max_output_bytes - output_bytes) {
            return fail("highlight.output_bytes", "完整转义HTML超出字节预算");
        }
        output_bytes += length;
    }
    for (std::size_t s = 0; s < result.spans.size(); ++s) {
        if (13 > limits.max_output_bytes - output_bytes) {
            return fail("highlight.output_bytes", "完整转义HTML含标签超出字节预算");
        }
        output_bytes += 13;
    }
    const auto capacity = base::checked_narrow<std::size_t>(output_bytes, "highlight.output_bytes");
    if (!capacity || capacity.value() > result.html.max_size()) {
        return fail("highlight.output_bytes", "完整HTML大小不可表示");
    }
    result.html.reserve(capacity.value());
    std::size_t span_index = 0;
    for (std::size_t b = 0; b < text.size(); ++b) {
        if (span_index < result.spans.size() && b == result.spans[span_index].byte_offset) {
            result.html += "<mark>";
        }
        const auto escaped = entity(text[b]);
        if (escaped.empty()) { result.html.push_back(text[b]); }
        else { result.html += escaped; }
        if (span_index < result.spans.size() && b + 1 ==
                result.spans[span_index].byte_offset + result.spans[span_index].byte_length) {
            result.html += "</mark>";
            ++span_index;
        }
    }
    return base::Result<Highlight>::success(std::move(result));
}
} // namespace siftwing::query
