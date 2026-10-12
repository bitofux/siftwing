/*
 * PROJECT : SIFTWING
 * FILE    : summary.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 严格完整准入后定位最早词键并映射回原UTF-8窗口
 * IMPLEMENTATION : 标量只用于定位；字节边界表用于复制原文，避免规范化偏移漂移。
 */
#include "siftwing/query/summary.h"
#include "siftwing/base/checked.h"
#include <utf8.h>
#include <algorithm>
#include <utility>
#include <vector>

namespace siftwing::query {
namespace {
using Points = std::vector<std::uint32_t>;
base::Result<Summary> fail(const char *context, std::string message) {
    return base::Result<Summary>::failure({std::move(message), context});
}
std::uint32_t fold_ascii(std::uint32_t point) noexcept {
    return point >= 'A' && point <= 'Z' ? point + ('a' - 'A') : point;
}
// bytes已经严格校验；标量数不大于已准入的uint64字节数，不在计数时分配。
std::uint64_t scalar_count(std::string_view bytes) {
    std::uint64_t count = 0;
    auto it = bytes.begin();
    while (it != bytes.end()) { (void)utf8::next(it, bytes.end()); ++count; }
    return count;
}
Points decode_key(std::string_view bytes) {
    Points points;
    auto it = bytes.begin();
    while (it != bytes.end()) { points.push_back(fold_ascii(utf8::next(it, bytes.end()))); }
    return points;
}
} // namespace

base::Result<Summary> make_summary(const Query &query, std::string_view content,
                                  SummaryOptions options, const SummaryLimits &limits) {
    if (limits.max_content_bytes == 0 || limits.max_query_terms == 0 ||
        limits.max_query_bytes == 0 || limits.max_match_pairs == 0 || limits.max_window_scalars == 0) {
        return fail("summary.limits", "摘要需要五项正预算");
    }
    if (options.window_scalars == 0 || options.window_scalars > limits.max_window_scalars ||
        options.leading_context_scalars >= options.window_scalars) {
        return fail("summary.options", "窗口须为正且不超预算，前文长度须小于窗口");
    }
    if (query.empty()) { return fail("summary.query", "移空查询不能生成摘要"); }
    const auto bytes = base::checked_narrow<std::uint64_t>(content.size(), "summary.content_bytes");
    if (!bytes || bytes.value() > limits.max_content_bytes) {
        return fail("summary.content_bytes", "完整正文超出字节预算");
    }
    const auto invalid = utf8::find_invalid(content.begin(), content.end());
    if (invalid != content.end()) {
        return fail("summary.utf8", "非法UTF-8原字节位置 " + std::to_string(invalid - content.begin()));
    }
    const auto nul = content.find('\0');
    if (nul != content.npos) { return fail("summary.nul", "NUL原字节位置 " + std::to_string(nul)); }
    const auto &terms = query.frequencies().terms;
    const auto count = base::checked_narrow<std::uint64_t>(terms.size(), "summary.query_terms");
    if (!count || count.value() > limits.max_query_terms) {
        return fail("summary.query_terms", "完整唯一查询词数超出预算");
    }
    std::uint64_t key_bytes = 0;
    std::uint64_t key_points = 0;
    for (const auto &term : terms) {
        const auto size = base::checked_narrow<std::uint64_t>(term.first.size(), "summary.query_bytes");
        if (!size || size.value() > limits.max_query_bytes - key_bytes) {
            return fail("summary.query_bytes", "完整唯一查询键累计字节超出预算");
        }
        key_bytes += size.value();
        // 有效Query的键非空、UTF-8合法；累计标量<=已检查的累计键字节。
        key_points += scalar_count(term.first);
    }
    const auto n = scalar_count(content);
    if (n > limits.max_match_pairs / key_points) {
        return fail("summary.work", "完整正文与全部查询键的标量匹配预算不足");
    }
    Points body;
    std::vector<std::size_t> boundaries{0};
    auto it = content.begin();
    while (it != content.end()) {
        body.push_back(fold_ascii(utf8::next(it, content.end())));
        boundaries.push_back(static_cast<std::size_t>(it - content.begin()));
    }
    std::vector<Points> keys;
    for (const auto &term : terms) { keys.push_back(decode_key(term.first)); }
    std::size_t hit = 0;
    bool matched = false;
    // 按正文位置遍历，先遇到的完整词键子串即是最早起点；完整工作准入已在任何早停前完成。
    for (std::size_t i = 0; i < body.size() && !matched; ++i) {
        for (const auto &key : keys) {
            if (key.size() > body.size() - i) { continue; }
            std::size_t j = 0;
            while (j < key.size() && body[i + j] == key[j]) { ++j; }
            if (j == key.size()) { hit = i; matched = true; break; }
        }
    }
    // min后的值均不超过真实正文长度，转换可表示；先减法再加窗口，避免巨大请求溢出。
    const auto width = static_cast<std::size_t>(std::min(n, options.window_scalars));
    const auto leading = static_cast<std::size_t>(std::min(n, options.leading_context_scalars));
    const auto desired = matched && hit > leading ? hit - leading : std::size_t{0};
    const auto start = std::min(desired, body.size() - width);
    const auto end = start + width;
    Summary result{std::string{content.substr(boundaries[start], boundaries[end] - boundaries[start])},
        static_cast<std::uint64_t>(boundaries[start]), matched, start != 0, end != body.size()};
    return base::Result<Summary>::success(std::move(result));
}
} // namespace siftwing::query
