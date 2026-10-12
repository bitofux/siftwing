/*
 * PROJECT : SIFTWING
 * FILE    : simhash.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 从完整单文档TF计算确定性64位近似指纹，不决定文档去留
 */
#pragma once

#include "siftwing/text/term_frequency.hpp"

#include <optional>

namespace siftwing::document {

/** @brief 特征、哈希、权重、平票与空输入规则的版本；不表示分词资源兼容。 */
inline constexpr std::uint32_t simhash_policy_version = 1;

/**
 * @brief 拥有64位指纹值，全部位型（包括0）合法；不代表DocumentId或内容唯一身份
 * @note 可复制/赋值，无外部资源；同对象有并发写时由调用者同步。
 */
struct SimHash64 final {
    std::uint64_t bits = 0; ///< 位0为最低位，不定义磁盘字节序；数值0不是空文档哨兵。
};

/** @brief 四项正预算，等于上限合法；约束现有TF的工作量，不是进程硬内存/CPU限额。 */
struct SimHashLimits final {
    std::uint64_t max_distinct_terms = 0;   ///< 输入TF唯一词数，不对词项做topN截断。
    std::uint64_t max_term_bytes = 0;       ///< 任一UTF-8词键字节，权重不乘入字节量。
    std::uint64_t max_vocabulary_bytes = 0; ///< 所有唯一词键字节之和，不含容器开销。
    std::uint64_t max_total_tokens = 0;     ///< 正TF之和，允许一直到UINT64_MAX。
};

/**
 * @brief 对过滤后完整TF作64位整数加权投票，不重新规范化、分词或过滤
 *
 * @param[in] frequencies
 *     同步借用单文档TF；调用期存活且无并发写。键须严格UTF-8、无NUL、非空、
 *     不含Unicode15.1 White_Space（同normalize），次数为正且总和等于total_tokens。
 *     调用方负责选择正文/标题、上游分词及停用配置；推荐累计或搜索DF不是本输入。
 * @param[in] limits
 *     同步借用四正预算；配置→唯一词数→词键字节预算→完整UTF8/NUL→
 *     键格式/正频/受检总和及总次数预算→投票。failure不交付部分指纹。
 *
 * @return
 *     成功拥有optional指纹；空表且总数0成功nullopt，不把空输入编码成数值0。
 *     非空表每词按UTF8字节FNV-1a64（offset=14695981039346656037、prime=1099511628211）
 *     后接fmix64（xor>>33、乘0xff51afd7ed558ccd、xor>>33、乘0xc4ceb9fe1a85ec53、
 *     xor>>33）；乘法按模2^64。以原始正TF投票，正票严格多于负票的位设1，平票0。
 *     context为simhash.limits/distinct_terms/term_bytes/vocabulary_bytes/utf8/nul/
 *     term/table/overflow/total_tokens。编码诊断含词键遍历序号和0基字节位置。
 *
 * @note 无std::hash、locale、浮点或文件IO；相同词键/TF及充分预算产生相同值，
 *     不依赖token顺序。非加密哈希，碰撞与近似误判可能发生，距离小不证明文本相同；
 *     未验证去重准确率。无共享可变状态，独立输入可独立调用；不修改Record或输入。
 *
 * @throws
 *     诊断分配和未知异常上传；局部栈状态自动释放，输入保持原值。
 */
[[nodiscard]] base::Result<std::optional<SimHash64>> make_simhash(
    const text::DocumentTermFrequencies &frequencies, const SimHashLimits &limits);

/**
 * @brief 按值计算两指纹不同位数，范围0—64，不判断重复、不内置阈值
 *
 * @param[in] lhs
 *     第一个拥有型数值。
 * @param[in] rhs
 *     第二个拥有型数值。
 *
 * @return
 *     XOR中1的数量；无分配/异常/IO，不接纳nullopt，空输入由调用方另行处理。
 */
[[nodiscard]] std::uint32_t hamming_distance(SimHash64 lhs, SimHash64 rhs) noexcept;

} // namespace siftwing::document
