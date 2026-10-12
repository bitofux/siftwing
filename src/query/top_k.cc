/*
 * PROJECT : SIFTWING
 * FILE    : top_k.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 结果条数准入后组合完整排序与拥有序列缩容
 * IMPLEMENTATION : 只在全部评分成功后截断，不改变完整准入和浮点次序。
 */
#include "siftwing/query/top_k.hpp"
#include <utility>

namespace siftwing::query {
base::Result<RankedDocuments> rank_top_k(
    const Query &query, const index::WeightedIndex &index, const TopKLimits &limits) {
    using Output = base::Result<RankedDocuments>;
    if (limits.max_top_k == 0) {
        return Output::failure({"结果条数上限必须为正", "top_k.limits"});
    }
    const auto k = query.top_k();
    if (k == 0 || k > limits.max_top_k) {
        return Output::failure({"请求条数超出结果预算", "top_k.top_k"});
    }
    auto ranked = rank_candidates(query, index, limits.ranking);
    if (!ranked) {
        return Output::failure(std::move(ranked).error());
    }
    auto documents = std::move(ranked).value();
    if (k < documents.size()) {
        // k小于已有容器长度才定位尾部；erase不要求强DocId具有默认构造，也不按巨大k分配。
        documents.erase(
            documents.begin() + static_cast<RankedDocuments::difference_type>(k), documents.end());
    }
    return Output::success(std::move(documents));
}
} // namespace siftwing::query
