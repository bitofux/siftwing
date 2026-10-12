/*
 * PROJECT : SIFTWING
 * FILE    : recommendation.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 共享Unicode字符召回后返回有界、独立拥有的候选词建议
 */
#pragma once
#include "siftwing/index/lexicon.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace siftwing::query {
/** @brief 八项正uint64预算，含端点；默认0拒绝，不是进程硬CPU/内存限制。 */
struct RecommendationLimits final {
    std::uint64_t max_query_bytes = 0;     ///< 完整输入词键UTF-8字节数。
    std::uint64_t max_query_scalars = 0;   ///< 完整输入Unicode标量数，重复逐次计入距离。
    std::uint64_t max_top_k = 0;           ///< 请求k的上限，不是候选数量，也不按k预分配。
    std::uint64_t max_character_links = 0; ///< 唯一查询字符的已知列表长度和，同词可跨列表重复计入。
    std::uint64_t max_candidates = 0;      ///< 共享字符列表并集的唯一词数，与k无关。
    std::uint64_t max_candidate_bytes = 0; ///< 全部唯一候选词键的UTF-8字节和，与k无关。
    std::uint64_t max_distance_cells = 0;  ///< 全部候选M*Li之和，仅计DP非边界单元。
    std::uint64_t max_output_bytes = 0;    ///< 返回前min(k,C)项拥有词文本的UTF-8字节和。
};

/**
 * @brief 一项拥有型建议；字段对应调用所用推荐词典，外部修改后调用方自维持字段关系
 * @note 可复制/移动/修改；文本不借用词典，但term_id仍只属于原推荐词典/快照。
 */
struct RecommendedTerm final {
    base::TermId term_id;    ///< 本推荐Lexicon局部身份，不能解释为搜索词ID或文档身份。
    std::string term;        ///< 独立拥有原词键，不进行规范化、HTML或JSON转义。
    std::uint64_t distance;  ///< Unicode标量Levenshtein距离，插入/删除/替换各1，交换非单步。
    std::uint64_t frequency; ///< 正推荐累计出现次数，不是搜索DF/N或单文档TF。
};
/** @brief 按距离升序、频次降序、UTF-8字节序升序的拥有前缀，修改后自行维护排序。 */
using RecommendedTerms = std::vector<RecommendedTerm>;

/**
 * @brief 在共享字符候选并集中计算全部距离并稳定取前k，不保证全词典最近
 * @param[in] key
 *     同步稳定借用单个词键，严格UTF-8、无NUL、非空、无Unicode15.1 White_Space。
 *     原大小写/标点/组合字符保留；调用方按建库兼容规则准备，不隐式normalize、NFC、
 *     casefold、分词或停用过滤。字符精确按标量相等，非字素簇。
 * @param[in] dictionary
 *     同步稳定借用有效推荐词典；空/移空词典或无共享字符成功空。查询重复字符只召回
 *     一次列表，未知字符忽略；候选去重、精确命中保留，无距离阈值或无共享词扫描。
 * @param[in] top_k
 *     [1,max_top_k]，等于上限合法，UINT64_MAX可用；输出恰min(k,C)，不补齐。
 * @param[in] limits
 *     顺序：八正配置→k→完整字节→完整UTF-8→NUL→词形→查询标量数→完整关系和→
 *     唯一候选数→全部候选字节和→全部M*Li工作→排序前缀→输出词字节和。
 *     完整工作准入先于任何距离计算，小k/精确命中不能绕过；未知/空候选仍检查输入。
 *
 * @return
 *     独立拥有建议或recommend.limits/top_k/query_bytes/utf8/nul/query/query_scalars/
 *     character_links/candidates/candidate_bytes/work/output_bytes诊断，无部分输出。
 * @throws
 *     分配及未知异常上传，局部RAII释放；无输入修改、借用逃逸、IO或共享可变缓存。
 * @pre
 *     调用期间输入不能被写入，词典不能与移动/赋值/析构并发；只读不延长所有者生命。
 * @note
 *     距离工作sum(M*Li)，完整排序O(C log(C+1))，另含索引查找/关系并集/字节准入。
 *     工作空间O(M+C+Lmax+H)，H为返回词字节；工作预算不计边界初始化/排序/容器开销。
 */
[[nodiscard]] base::Result<RecommendedTerms> recommend_terms(std::string_view key,
    const index::RecommendationDictionary &dictionary,
    std::uint64_t top_k,
    const RecommendationLimits &limits);
} // namespace siftwing::query
