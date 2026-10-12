/*
 * PROJECT : SIFTWING
 * FILE    : candidates.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 从查询词约束与有效倒排生成拥有型候选文档集合
 */
#pragma once
#include "siftwing/index/inverted_index.hpp"
#include "siftwing/query/query.hpp"

namespace siftwing::query {
/** @brief 拥有文档身份的可变值序列；生成时严格递增且唯一，调用方修改后不再保证此性质。 */
using CandidateDocuments = std::vector<base::DocumentId>;

/** @brief 三项正预算，含端点；控制准入与输出数量，不是进程内存或CPU硬上限。 */
struct CandidateLimits final {
    std::uint64_t max_query_terms = 0; ///< 查询唯一词键数，未知词也计入，重复次数不计。
    std::uint64_t max_postings = 0;    ///< 所有已知查询词的完整Posting列表长度总和，合并前准入。
    std::uint64_t max_candidates = 0;  ///< 最终唯一文档数，不限制AND中间匹配或按top_k截断。
};

/**
 * @brief 将唯一查询词键映射到本索引，执行AND交集或OR并集
 *
 * @param[in] query
 *     同步稳定借用工厂生成的有效Query；移空源拒绝。重复TF不增加约束，top_k不参与合并。
 * @param[in] index
 *     同步稳定借用有效InvertedIndex（包括空或移空索引）。TermId只从此索引的词表取得，
 *     Posting严格递增/非空由索引保证，不接受外部可变裸列表。调用方保证建库与查询资源相容。
 * @param[in] limits
 *     先三项正配置、非空Query、唯一词数，再按词键顺序核所有已知列表的总准入，最后合并。
 *     AND含未知词也先检查全部已知列表准入；成功空集合要求同样的正配置。
 *
 * @return
 *     独立拥有、按DocId递增且唯一的完整集合；0/UINT64_MAX是普通身份，不按数值作数组下标。
 *     AND任一未知词为空，OR忽略未知词，全未知/空索引均成功空集合；不同于移空Query错误。
 *     failure只有candidate.limits/query/terms/postings/documents诊断，无部分结果，输入不变。
 *
 * @note 输出不借用Query或索引，调用结束后可独立复制/移动/修改。输入不能与所有者移动、
 *     赋值或析构并发；无IO、打分、排序、共享可变缓存或并发专项验证。
 * @throws 分配及未知异常上传，局部容器RAII释放；准入计完整列表条数，不是实际比较次数。
 */
[[nodiscard]] base::Result<CandidateDocuments> collect_candidates(
    const Query &query, const index::InvertedIndex &index, const CandidateLimits &limits);
} // namespace siftwing::query
