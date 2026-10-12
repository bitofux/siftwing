/*
 * PROJECT : SIFTWING
 * FILE    : recommendation.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 完整候选与距离工作准入后计算Unicode词距、排序并复制拥有前缀
 * IMPLEMENTATION : 字符只召回一次列表，候选只评分一次；DP单行保留旧对角，借用不逃逸。
 */
#include "siftwing/query/recommendation.hpp"
#include "siftwing/base/checked.hpp"
#include <utf8.h>
#include <algorithm>
#include <set>
#include <utility>

namespace siftwing::query {
namespace {
using Points = std::vector<std::uint32_t>;
base::Result<RecommendedTerms> fail(const char *context, std::string message) {
    return base::Result<RecommendedTerms>::failure({std::move(message), context});
}
// 与已封板Lexicon固定的Unicode15.1 White_Space集合一致，不依赖locale。
bool whitespace(std::uint32_t cp) noexcept {
    return (cp >= 9U && cp <= 13U) || cp == 0x20U || cp == 0x85U || cp == 0xa0U || cp == 0x1680U
           || (cp >= 0x2000U && cp <= 0x200aU) || cp == 0x2028U || cp == 0x2029U || cp == 0x202fU
           || cp == 0x205fU || cp == 0x3000U;
}
std::uint64_t scalar_count(std::string_view word) {
    std::uint64_t count = 0;
    auto it = word.begin();
    while (it != word.end()) {
        (void)utf8::next(it, word.end());
        ++count;
    }
    return count;
}
Points decode(std::string_view word) {
    Points points;
    auto it = word.begin();
    while (it != word.end()) {
        points.push_back(utf8::next(it, word.end()));
    }
    return points;
}
std::uint64_t edit_distance(const Points &first, const Points &second) {
    const auto &columns = first.size() <= second.size() ? first : second;
    const auto &rows = first.size() <= second.size() ? second : first;
    std::vector<std::uint64_t> row(columns.size() + 1);
    for (std::size_t j = 0; j < row.size(); ++j) {
        row[j] = static_cast<std::uint64_t>(j);
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto diagonal = row[0];
        row[0] = static_cast<std::uint64_t>(i) + 1;
        for (std::size_t j = 0; j < columns.size(); ++j) {
            const auto above = row[j + 1];
            // row[j]是新行左值，above/diagonal是旧行值；成功构造的Points长度受
            // vector最大容量约束，距离<=max(M,L)，下述+1不会达到uint64回绕。
            row[j + 1] = std::min({above + 1,
                row[j] + 1,
                diagonal + (rows[i] == columns[j] ? std::uint64_t{0} : std::uint64_t{1})});
            diagonal = above;
        }
    }
    return row.back();
}
struct Scored final {
    base::TermId id;
    std::uint64_t distance;
};
} // namespace

base::Result<RecommendedTerms> recommend_terms(std::string_view key,
    const index::RecommendationDictionary &dictionary,
    std::uint64_t top_k,
    const RecommendationLimits &limits) {
    if (!limits.max_query_bytes || !limits.max_query_scalars || !limits.max_top_k
        || !limits.max_character_links || !limits.max_candidates || !limits.max_candidate_bytes
        || !limits.max_distance_cells || !limits.max_output_bytes) {
        return fail("recommend.limits", "推荐需要八项正预算");
    }
    if (!top_k || top_k > limits.max_top_k) {
        return fail("recommend.top_k", "请求k超出合法范围");
    }
    const auto size = base::checked_narrow<std::uint64_t>(key.size(), "recommend.query_bytes");
    if (!size || size.value() > limits.max_query_bytes) {
        return fail("recommend.query_bytes", "完整词键超出字节预算");
    }
    const auto invalid = utf8::find_invalid(key.begin(), key.end());
    if (invalid != key.end()) {
        return fail(
            "recommend.utf8", "非法UTF-8原字节位置 " + std::to_string(invalid - key.begin()));
    }
    const auto nul = key.find('\0');
    if (nul != key.npos) {
        return fail("recommend.nul", "NUL原字节位置 " + std::to_string(nul));
    }
    if (key.empty()) {
        return fail("recommend.query", "推荐词键须非空且无空白");
    }
    std::uint64_t m = 0;
    auto cursor = key.begin();
    while (cursor != key.end()) {
        if (whitespace(utf8::next(cursor, key.end()))) {
            return fail("recommend.query", "推荐词键须非空且无空白");
        }
        ++m;
    }
    if (m > limits.max_query_scalars) {
        return fail("recommend.query_scalars", "完整词键标量超限");
    }
    const auto points = decode(key);
    std::set<char32_t> unique;
    for (auto cp : points) {
        unique.insert(static_cast<char32_t>(cp));
    }
    std::vector<const std::vector<base::TermId> *> lists;
    std::uint64_t links = 0;
    for (auto cp : unique) {
        const auto it = dictionary.characters().find(cp);
        if (it == dictionary.characters().end()) {
            continue;
        }
        const auto count =
            base::checked_narrow<std::uint64_t>(it->second.size(), "recommend.character_links");
        if (!count || count.value() > limits.max_character_links - links) {
            return fail("recommend.character_links", "完整字符关系列表超限");
        }
        links += count.value();
        lists.push_back(&it->second);
    }
    std::set<base::TermId> candidates;
    for (const auto *list : lists) {
        for (auto id : *list) {
            if (candidates.find(id) != candidates.end()) {
                continue;
            }
            if (candidates.size() >= limits.max_candidates) {
                return fail("recommend.candidates", "完整唯一候选数超限");
            }
            candidates.insert(id);
        }
    }
    const auto &terms = dictionary.lexicon().terms();
    const auto &frequencies = dictionary.frequencies();
    std::uint64_t bytes = 0;
    for (auto id : candidates) {
        const auto count = base::checked_narrow<std::uint64_t>(
            terms[static_cast<std::size_t>(id.value())].size(), "recommend.candidate_bytes");
        if (!count || count.value() > limits.max_candidate_bytes - bytes) {
            return fail("recommend.candidate_bytes", "全部候选词键字节超限");
        }
        bytes += count.value();
    }
    std::uint64_t work = 0;
    for (auto id : candidates) {
        const auto n = scalar_count(terms[static_cast<std::size_t>(id.value())]);
        // m>0；先除法准入再乘积/累加，完整检查在任何DP计算之前。
        if (n > (limits.max_distance_cells - work) / m) {
            return fail("recommend.work", "全部候选标量距离工作超限");
        }
        work += m * n;
    }
    std::vector<Scored> scored;
    for (auto id : candidates) {
        const auto word = decode(terms[static_cast<std::size_t>(id.value())]);
        scored.push_back({id, edit_distance(points, word)});
    }
    std::sort(scored.begin(), scored.end(), [&](const Scored &a, const Scored &b) {
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        const auto af = frequencies[static_cast<std::size_t>(a.id.value())];
        const auto bf = frequencies[static_cast<std::size_t>(b.id.value())];
        if (af != bf) {
            return af > bf;
        }
        // 有效Lexicon的ID恰按UTF-8字节序排列，故此处ID升序就是最终词键字节序。
        return a.id < b.id;
    });
    const auto selected = top_k < scored.size() ? static_cast<std::size_t>(top_k) : scored.size();
    std::uint64_t output_bytes = 0;
    for (std::size_t i = 0; i < selected; ++i) {
        const auto length = static_cast<std::uint64_t>(
            terms[static_cast<std::size_t>(scored[i].id.value())].size());
        if (length > limits.max_output_bytes - output_bytes) {
            return fail("recommend.output_bytes", "返回词文本字节超限");
        }
        output_bytes += length;
    }
    RecommendedTerms result;
    for (std::size_t i = 0; i < selected; ++i) {
        const auto &item = scored[i];
        const auto offset = static_cast<std::size_t>(item.id.value());
        result.push_back({item.id, terms[offset], item.distance, frequencies[offset]});
    }
    return base::Result<RecommendedTerms>::success(std::move(result));
}
} // namespace siftwing::query
