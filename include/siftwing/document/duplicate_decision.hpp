/*
 * PROJECT : SIFTWING
 * FILE    : duplicate_decision.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 独立于文档记录的确定性、有限额批量去重决策与审计报告
 */
#pragma once

#include "siftwing/document/document_record.hpp"
#include "siftwing/document/simhash.hpp"

#include <vector>

namespace siftwing::document {

/** @brief 拥有身份、来源定位与可缺席指纹；不拥有正文，不在此计算或验证指纹资源兼容。 */
struct DuplicateCandidate final {
    base::DocumentId doc_id;              ///< 显式身份；0合法，同一批须唯一，不用数值决定保留顺序。
    SourceIdentity source_id;             ///< 拥有来源名及文件内序号；同一批须唯一。
    std::optional<SimHash64> fingerprint; ///< nullopt仅表示上游成功但没有特征；不能代替分析失败。
};

/** @brief 拥有本批策略；仅与已保留者比较、最早命中、非传递聚类是固定规则。 */
struct DuplicatePolicy final {
    std::uint32_t max_hamming_distance = 3; ///< 0—64含端点；距离等于阈值也判为重复。
};

/** @brief 四项正预算；计量决策层工作量，不是进程硬内存/CPU上限。 */
struct DuplicateLimits final {
    std::uint64_t max_documents = 0;               ///< 所有候选条数，包括无特征；0表示未配置。
    std::uint64_t max_source_path_bytes = 0;       ///< 每个来源标识的原字节数，不含序号。
    std::uint64_t max_total_source_path_bytes = 0; ///< 每个候选计一次来源字节，不按文件去重。
    std::uint64_t max_comparisons = 0; ///< 实际执行的指纹对比较数；命中后停止，不计排序比较。
};

/** @brief 候选的三种决策；数值不构成持久化编码。 */
enum class DuplicateDisposition {
    kept,       ///< 有指纹且未命中；进入后续保留集合。
    duplicate,  ///< 命中最早保留者；本候选不参与后续比较。
    no_features ///< 无指纹；跳过搜索保留集合，与读取失败和重复均不同。
};

/** @brief 拥有型重复依据；只引用本批先前已经保留的身份。 */
struct DuplicateMatch final {
    base::DocumentId retained_id; ///< 最早命中的保留文档，不一定是距离最近或DocId最小者。
    std::uint32_t distance = 0;   ///< 实际汉明距离，0—64且不大于本批阈值。
};

/** @brief 一条拥有型决策；不保存候选引用或正文，不修改DocumentRecord。 */
struct DuplicateDecision final {
    base::DocumentId doc_id;  ///< 本候选身份，保留原值，不重新编号。
    SourceIdentity source_id; ///< 来源副本，输入销毁后仍可定位。
    DuplicateDisposition disposition = DuplicateDisposition::no_features; ///< 本候选结果。
    std::optional<DuplicateMatch> match; ///< 当且仅当duplicate存在；其他结果没有匹配依据。
};

/** @brief 成功报告的审计统计；输入条数等于kept+duplicates+no_features。 */
struct DuplicateCounts final {
    std::uint64_t input = 0;       ///< 全部候选数。
    std::uint64_t kept = 0;        ///< 有指纹且保留的数量。
    std::uint64_t duplicates = 0;  ///< 被丢弃的相似候选数量。
    std::uint64_t no_features = 0; ///< 无特征候选数量。
    std::uint64_t comparisons = 0; ///< 已执行的指纹比较数量，不计排序/身份校验。
};

/** @brief 成功交付的完整拥有型报告；所有成员可修改，修改后的报告不由函数重新验证。 */
struct DuplicateReport final {
    DuplicatePolicy policy; ///< 实际使用的策略副本；本报告不负责快照清单序列化。
    std::vector<DuplicateDecision>
        decisions;          ///< 来源名std::string字典序、序号升序，与输入排列无关。
    DuplicateCounts counts; ///< 对全部成功决策计数，无提前停止或隐藏的部分结果。
};

/**
 * @brief 完整预检后按规范来源次序生成非传递、最早保留者匹配的批量去重决策
 *
 * @param[in] candidates
 *     同步借用；调用期间存活且无并发写。来源名遵守读取层相对标识规则：非空、
 *     非/开头，无NUL、空段、.或..段，不访问文件、不规范化、不额外要求UTF-8。
 *     DocId和(来源名,序号)分别须唯一；指纹规则/上游资源兼容由调用方保证。
 * @param[in] policy
 *     按值取得阈值，0—64合法，默认3。
 * @param[in] limits
 *     同步借用四正预算，等于上限合法。先检查配置、条数、来源字节与路径、
 *     唯一DocId及来源，再处理；比较预算在每次距离计算前检查。
 *
 * @return
 *     成功拥有完整报告，空批量合法。nullopt单列no_features，不参加比较；数值0
 *     是合法指纹。仅与先前kept者比较，第一处距离≤阈值即duplicate；丢弃者不成为
 *     后续匹配依据。context为duplicate.policy/limits/documents/source_path_bytes/
 *     total_source_path_bytes/source/doc_id/comparisons/overflow；失败不交付部分报告。
 *
 * @note 输入不改，报告不借用输入；无IO或共享可变状态。独立只读输入可独立调用，
 *     外部输入/输出写入或销毁须同步。排序O(n log n)，朴素距离最坏O(n²)，
 *     比较预算限制实际距离次数，不追回已装载输入或分配开销。小距离仅是规则命中，
 *     不证明文本相同/去重准确率；不通过丢弃者形成传递聚类。
 *
 * @throws
 *     分配和未知异常原样上传；局部拥有状态RAII释放，输入保持。
 */
[[nodiscard]] base::Result<DuplicateReport> decide_duplicates(
    const std::vector<DuplicateCandidate> &candidates,
    DuplicatePolicy policy,
    const DuplicateLimits &limits);

} // namespace siftwing::document
