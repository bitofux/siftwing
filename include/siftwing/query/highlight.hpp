/*
 * PROJECT : SIFTWING
 * FILE    : highlight.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 定义原文字节命中并集及安全HTML正文高亮合同
 */
#pragma once
#include "siftwing/query/query.hpp"
#include <vector>

namespace siftwing::query {
/** @brief 相对本次输入文字的原UTF-8字节区间，不是HTML或完整正文中的偏移。 */
struct HighlightSpan final {
    std::uint64_t byte_offset = 0; ///< 0基原字节起点，位于Unicode标量边界。
    std::uint64_t byte_length = 0; ///< 正原字节长度，末端位于标量边界。
};
/** @brief 六项显式正预算，含端点；不提供整个进程CPU/内存硬限制。 */
struct HighlightLimits final {
    std::uint64_t max_text_bytes = 0;   ///< 完整输入原字节上限。
    std::uint64_t max_query_terms = 0;  ///< 全部唯一保留查询词上限，含索引未知词。
    std::uint64_t max_query_bytes = 0;  ///< 全部唯一词键累计UTF-8字节。
    std::uint64_t max_match_pairs = 0;  ///< 输入标量N乘全部唯一键累计标量Q的完整准入。
    std::uint64_t max_spans = 0;        ///< 重叠及相邻合并后的区间数量上限。
    std::uint64_t max_output_bytes = 0; ///< 最终HTML字节，包含所有实体与每对13字节mark标签。
};
/**
 * @brief 独立拥有HTML正文片段及严格分离、有序、正长度的原文字节区间
 * @note 可复制/移动/修改；外部修改后由调用方维持HTML与区间关系。
 */
struct Highlight final {
    std::string html; ///< 原文转义后加固定mark标签；无输入借用，不是JSON序列化结果。
    std::vector<HighlightSpan> spans; ///< 原文命中区间并集，重叠与相邻均合并。
};
/**
 * @brief 按原文全部词键命中并集合并区间，转义后生成固定mark高亮
 *
 * @param[in] query
 *     同步稳定借用有效Query，移空拒绝。全部唯一键参与，与TF次数、mode、top_k无关。
 * @param[in] text
 *     同步稳定借用完整严格UTF-8、无NUL文字，可为Summary.text或显式原文；空成功空。
 *     不读取Reader/索引，不延用Summary的偏移或matched，不修改输入。
 * @param[in] limits
 *     顺序为六正配置→有效Query→总字节→完整UTF-8→NUL→唯一词数/累计键字节→N*Q
 *     完整工作准入→合并区间数→最终HTML字节；无命中/空文字仍检查适用的查询预算。
 *
 * @return
 *     独立拥有Highlight，或highlight.limits/query/text_bytes/utf8/nul/query_terms/
 *     query_bytes/work/spans/output_bytes诊断，无部分输出；编码/NUL含0基原字节位置。
 *
 * @note 正文/键仅ASCII A—Z折小写匹配，其余Unicode标量精确子串；穷举全部起点，
 *     允许重叠，合并相邻区间。无词边界、NFC、完整casefold或字素簇保证。
 *     原文字节全部转义：& < > 双引号 单引号→&amp; &lt; &gt; &quot; &#39;，区间外添加
 *     固定<mark>/</mark>；无命中也转义，不识别已有实体或HTML，不允许自定义标签。
 *     输出仅用于HTML正文文本位置，不能用于属性/URL/JS/CSS；JSON转义由序列化层负责。
 *     再次输入html会再次转义，不承诺幂等；截短摘要可能不包含完整查询词。
 *     最坏O(N*Q+B+H)，额外O(N+Q+S+H)，B原字节、H输出字节、S合并区间数。
 *     无IO/共享缓存，借用不逃逸；输入/预算不能并发写或与所有者移动/析构并发。
 * @throws 分配及未知异常上传，局部拥有资源由RAII释放。
 */
[[nodiscard]] base::Result<Highlight> make_highlight(
    const Query &query, std::string_view text, const HighlightLimits &limits);
} // namespace siftwing::query
