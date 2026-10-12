/*
 * PROJECT : SIFTWING
 * FILE    : summary.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 定义保持原文UTF-8字节的动态摘要窗口与完整匹配准入
 */
#pragma once
#include "siftwing/query/query.h"

namespace siftwing::query {
/** @brief 标量窗口选项；不是字节、字素簇或词的数量。 */
struct SummaryOptions final {
    std::uint64_t window_scalars = 160; ///< 正窗口长度，含命中起点，须不超出窗口预算。
    std::uint64_t leading_context_scalars = 40; ///< 期望的命中前文长度，可为0，须小于窗口。
};
/** @brief 五项显式正预算，含端点；不限制整个进程的内存或CPU。 */
struct SummaryLimits final {
    std::uint64_t max_content_bytes = 0; ///< 完整原正文UTF-8字节，截取前计量。
    std::uint64_t max_query_terms = 0; ///< 全部保留的唯一查询词数，不重复计TF次数。
    std::uint64_t max_query_bytes = 0; ///< 全部唯一查询键累计UTF-8字节。
    std::uint64_t max_match_pairs = 0; ///< 正文标量N乘全部唯一查询键累计标量Q，完整准入。
    std::uint64_t max_window_scalars = 0; ///< 请求窗口上限；短正文不使非法请求合法。
};
/**
 * @brief 独立拥有原正文连续窗口；可复制/移动/修改，修改后由调用方维护元数据关系
 * @note 不附加省略号、标题、HTML或JSON转义；缺失正文以空串交付。
 */
struct Summary final {
    std::string text; ///< 原文字节副本，保留大小写、换行和空白，没有借用逃逸。
    std::uint64_t source_byte_offset = 0; ///< text起点在原正文中的0基字节偏移，空正文为0。
    bool matched = false; ///< 完整正文是否含任一词键子串，不保证长词完整落在短窗口内。
    bool truncated_before = false; ///< 原正文是否有未返回的前缀，不等于是否存在省略号。
    bool truncated_after = false; ///< 原正文是否有未返回的后缀。
};
/**
 * @brief 在最早查询词位置周围截取标量窗口，无命中取正文开头
 *
 * @param[in] query
 *     同步稳定借用有效Query；移空拒绝。全部唯一键参与，与次数、mode和top_k无关。
 * @param[in] content
 *     同步稳定借用原正文，完整严格UTF-8且无NUL；空表示缺失正文，成功返回空摘要。
 *     调用方负责将排名身份映射到同快照正文，本函数不访问索引、标题或Reader。
 * @param[in] options
 *     正窗口与小于窗口的前文长度。N为正文标量数，W=min(N,window_scalars)。命中h时
 *     s=min(max(0,h-leading_context_scalars),N-W)，无命中s=0；返回[s,s+W)。
 * @param[in] limits
 *     依次检查五正预算、选项、有效Query、正文总字节、完整UTF-8/NUL、唯一查询词数/
 *     累计键字节及N*Q完整预算；不以小窗口、空正文或早期命中跳过对应准入。
 *
 * @return
 *     独立拥有Summary或summary.limits/options/query/content_bytes/utf8/nul/query_terms/
 *     query_bytes/work诊断，无部分摘要；编码/NUL诊断包含0基原字节位置。
 *
 * @note 定位只将正文/词键ASCII A—Z折小写，其他Unicode标量精确匹配；允许重叠子串，
 *     不保证词边界（he可匹配the），不重新分词或做NFC/完整casefold/空白折叠。
 *     标量边界不保证字素簇或emoji序列完整。显示仍保留原文字节，高亮另由调用层完成。
 *     最坏匹配O(N*Q)，验证/解码O(正文及键字节)，额外空间O(N+Q)，输出最多4W字节。
 *     不访问IO或共享缓存；输入/预算不得并发写或与所有者移动/赋值/析构并发。
 * @throws 分配及未知异常上传；所有局部拥有资源由RAII释放。
 */
[[nodiscard]] base::Result<Summary> make_summary(const Query &query, std::string_view content,
    SummaryOptions options, const SummaryLimits &limits);
} // namespace siftwing::query
