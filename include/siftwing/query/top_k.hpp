/*
 * PROJECT : SIFTWING
 * FILE    : top_k.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 完整余弦排序的稳定前缀与显式结果条数预算
 */
#pragma once
#include "siftwing/query/ranking.hpp"

namespace siftwing::query {
/** @brief 排序预算与结果条数上限，所有端点含上限，不是进程硬资源限制。 */
struct TopKLimits final {
    RankingLimits ranking;       ///< 原完整候选/评分四正预算，小k不能绕过这些准入。
    std::uint64_t max_top_k = 0; ///< 显式正请求条数上限；0拒绝，不隐式缩小查询k。
};
/**
 * @brief 返回同输入完整余弦排序的前min(Query::top_k(),候选数)条
 *
 * @param[in] query
 *     稳定同步借用有效Query；k须在[1,limits.max_top_k]，移空源沿candidate.query拒绝。
 *
 * @param[in] index
 *     稳定同步借用WeightedIndex；查询/建库资源相容由调用方保证，无输入修改或借用逃逸。
 *
 * @param[in] limits
 *     先检查正max_top_k，再请求k，随后原样执行完整rank_candidates的全部检查。
 *     空结果和小k均不能绕过原排序配置、舍入环境、全部候选及C*T预算。
 *
 * @return
 *     独立拥有RankedDocuments，分数降序、精确同分DocId升序；分数逐位保持，不重新评分。
 *     未知词、空/移空索引、重复查询词沿排序合同；成功空无需虚构文档，k大于命中数不补齐。
 *     failure为top_k.limits/top_k.top_k或原ranking/candidate诊断，无部分输出。
 *
 * @note 输入不得与所有者移动/赋值/析构并发。输出可复制/移动/修改，修改者维护不变量。
 *     仍完整评分与排序，成本/峰值内存沿ranking；返回少不表示仅评分k篇或O(k)峰值内存。
 *     无IO/共享缓存，新函数不比较浮点；数值确定性与跨平台边界沿原排序。
 * @throws 分配与未知异常上传；所有局部拥有资源由RAII释放。
 */
[[nodiscard]] base::Result<RankedDocuments> rank_top_k(
    const Query &query, const index::WeightedIndex &index, const TopKLimits &limits);
} // namespace siftwing::query
