/*
 * PROJECT : SIFTWING
 * FILE    : ranking.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 同索引候选的TF-IDF查询向量、余弦分数与完整排序
 */
#pragma once
#include "siftwing/query/candidates.h"
#include "siftwing/index/weighted_index.h"
#include <cstdint>
#include <vector>

namespace siftwing::query {
/** @brief 拥有文档身份和相关性；发布后可修改，修改者负责重新维持分数与排序性质。 */
struct RankedDocument final {
    base::DocumentId document_id; ///< 命中当前索引的强身份，不借用文档；0/MAX及稀疏值合法。
    double score; ///< 发布时有限且0<score<=1的单位向量内积，最后夹到数学上界1。
};
/** @brief 完整拥有的分数降序序列；精确同分按DocId升序，无epsilon合并或top_k截断。 */
using RankedDocuments = std::vector<RankedDocument>;
/** @brief 四项正预算，含端点；计准入组合与输出条数，不是硬CPU或内存限。 */
struct RankingLimits final {
    CandidateLimits candidates; ///< 沿候选合同计唯一词、完整Posting准入及最终文档数。
    std::uint64_t max_score_pairs = 0; ///< 候选数C乘已知唯一查询词数T，含未命中维度的二分探测。
};
/**
 * @brief 内部生成同索引完整候选，再按TF-IDF余弦分数完整排序
 *
 * @param[in] query
 *     稳定同步借用有效Query，移空源拒绝；重复TF影响向量方向，top_k不截断输出。
 *
 * @param[in] index
 *     稳定同步借用WeightedIndex，内部只从其raw_index查局部TermId与候选，不接外部候选。
 *     调用方保证查询与建库管线相容；空/移空索引合法，不校验资源声明真实性。
 *
 * @param[in] limits
 *     先全部四项正配置，再FE_TONEAREST、collect_candidates、C*T准入，最后数值与排序。
 *     候选错误原样保留，空候选成功空且不构造零范数向量，但仍需正配置和正确浮点环境。
 *
 * @return
 *     拥有完整RankedDocuments；分数降序、精确同分DocId升序。仅已知词的原TF乘本索引IDF，
 *     按TermId递增hypot累计查询范数并归一化，与同索引已归一化文档权重按该次序点积。
 *     OR未知不进入查询范数，AND未知为空；文档未查询词仍影响已有文档范数。
 *     正有限点积以min(1,score)夹到数学范围，不用容差合并不同分数或文档。
 *     failure为ranking.limits/rounding/work/numeric或candidate.*，无部分结果，输入不改。
 *
 * @note 输出无借用，可独立复制/移动/修改。同步调用不能与输入所有者移动/赋值/析构并发；
 *     无IO、共享可变缓存或并发专项验证。沿权重IEEE754 binary64及严格浮点前提，
 *     同输入/策略/环境确定，跨架构/libm不承诺逐位相同；函数检测而不改变浮点舍入模式。
 * @throws 分配及未知异常上传，局部容器RAII释放；预算不是二分实际比较次数或进程硬资源限。
 */
[[nodiscard]] base::Result<RankedDocuments> rank_candidates(
    const Query &query, const index::WeightedIndex &index, const RankingLimits &limits);
} // namespace siftwing::query
