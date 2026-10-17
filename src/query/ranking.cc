/*
 * PROJECT : SIFTWING
 * FILE    : ranking.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 已知查询维度L2归一化与同索引候选点积排序
 * IMPLEMENTATION : 维度按TermId固定累积次序，Posting位置与权重对齐，稀疏身份只用于二分。
 */
#include "siftwing/query/ranking.hpp"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <limits>
#include <utility>

namespace siftwing::query {
namespace {
using Output = base::Result<RankedDocuments>;
Output fail(const char *context, const char *message) {
    return Output::failure({message, context});
}
struct Dimension final {
    base::TermId term; // 拥有本索引局部身份，排序固定查询范数及点积的累积次序。
    double weight;                      // 先为TF*IDF，随后原位替换为单位查询权重。
    const index::PostingList *postings; // 同步借用，不逃逸。
    const std::vector<double> *weights; // 与postings逐位置对齐，由同一WeightedIndex保证。
};
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53
                  && std::numeric_limits<double>::max_exponent == 1024,
    "ranking requires IEEE754 binary64");
} // namespace

base::Result<RankedDocuments> rank_candidates(
    const Query &query, const index::WeightedIndex &index, const RankingLimits &limits) {
    const auto &budget = limits.candidates;
    if (!budget.max_query_terms || !budget.max_postings || !budget.max_candidates
        || !limits.max_score_pairs) {
        return fail("ranking.limits", "候选和评分四项预算必须为正");
    }
    if (std::fegetround() != FE_TONEAREST) {
        return fail("ranking.rounding", "余弦计算要求FE_TONEAREST");
    }
    const auto &raw = index.raw_index();
    auto candidates = collect_candidates(query, raw, budget);
    if (!candidates) {
        return Output::failure(std::move(candidates).error());
    }
    if (candidates.value().empty()) {
        return Output::success({});
    }

    // 非空候选必有至少一个已知词，所以known>0；先除后比较，C*T不先相乘。
    // 未知OR词不进入范数或评分组合；known不超过已有Query容器大小。
    std::uint64_t known = 0;
    for (const auto &term : query.frequencies().terms) {
        if (raw.lexicon().find(term.first)) {
            ++known;
        }
    }
    if (candidates.value().size() > limits.max_score_pairs / known) {
        return fail("ranking.work", "候选与已知查询维度组合数超出预算");
    }
    std::vector<Dimension> dimensions;
    dimensions.reserve(static_cast<std::size_t>(known));
    for (const auto &term : query.frequencies().terms) {
        const auto id = raw.lexicon().find(term.first);
        if (!id) {
            continue;
        }
        const auto weight =
            static_cast<double>(term.second) * *index.inverse_document_frequency(*id);
        if (!std::isfinite(weight) || weight <= 0) {
            return fail("ranking.numeric", "查询TF-IDF必须为正有限值");
        }
        dimensions.push_back({*id, weight, raw.postings(*id), index.weights(*id)});
    }
    std::sort(dimensions.begin(), dimensions.end(), [](const auto &a, const auto &b) {
        return a.term < b.term;
    });
    double norm = 0;
    for (const auto &dimension : dimensions) {
        norm = std::hypot(norm, dimension.weight);
    }
    if (!std::isfinite(norm) || norm <= 0) {
        return fail("ranking.numeric", "非空候选查询范数必须为正有限值");
    }
    for (auto &dimension : dimensions) {
        dimension.weight /= norm;
    }

    RankedDocuments result;
    result.reserve(candidates.value().size());
    for (auto document : candidates.value()) {
        double score = 0;
        for (const auto &dimension : dimensions) {
            const auto &postings = *dimension.postings;
            const auto it = std::lower_bound(postings.begin(),
                postings.end(),
                document,
                [](const auto &posting, base::DocumentId key) {
                    return posting.document_id() < key;
                });
            if (it != postings.end() && it->document_id() == document) {
                const auto position = static_cast<std::size_t>(it - postings.begin());
                score += dimension.weight * (*dimension.weights)[position];
            }
        }
        // 数学内积在(0,1]；固定次序浮点求和可略超1。仅夹数学上界，不以容差改变排序。
        if (!std::isfinite(score) || score <= 0) {
            return fail("ranking.numeric", "命中文档分数必须为正有限值");
        }
        result.push_back({document, std::min(1.0, score)});
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
    });
    return Output::success(std::move(result));
}
} // namespace siftwing::query
