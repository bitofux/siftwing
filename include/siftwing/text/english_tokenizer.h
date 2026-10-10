/*
 * PROJECT : SIFTWING
 * FILE    : english_tokenizer.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 定义无locale依赖的ASCII英文及数字分词规则
 */

#pragma once

#include "siftwing/text/tokenizer.h"

namespace siftwing::text {

/** @brief 仅英文实现的固定规则版本；不是全部Tokenizer或未来快照的版本。 */
inline constexpr std::uint32_t english_tokenization_policy_version = 1;

/**
 * @brief 按最大连续ASCII字母/数字段切词的无状态实现
 *
 * @note [A-Za-z0-9]+为token，其他字符作为分隔；不产生空token，保留源顺序和重复。
 *     原大小写保留，规范化由调用方显式执行。撇号、连字符、下划线和小数点均分隔，
 *     如don't→don/t，foo_bar→foo/bar，3.14→3/14；数字不丢弃，a1为一个token。
 *     所有非ASCII也分隔，如café→caf、foo中文bar→foo/bar；不音译、不合并相邻两侧，
 *     不声称Unicode词边界或中文分词。U+FEFF无论位置都作为分隔符。
 * @note 无成员资源或共享可变状态，同一只读实例可独立并发调用；外部input/limits不被
 *     并发写入仍是前置条件。复制/移动实例不携带调用状态。
 */
class EnglishTokenizer final : public Tokenizer {
public:
    /**
     * @brief 按规则版本1生成拥有序列，沿用Tokenizer输入/输出/异常合同
     *
     * @param[in] input
     *     同步借用严格UTF-8文字，原字节不修改；可以是显式规范化后的字符串。
     * @param[in] limits
     *     四项正预算；完整UTF-8/NUL验证优先于任何输出预算。
     *
     * @return
     *     全部token或拥有诊断；空/仅分隔符/纯非ASCII成功为空序列。按源token顺序检查
     *     单token字节→token条数→累计字节，首次超限整次失败，不发布此前token。
     *
     * @note 输出预算在分配序列和复制token前完整预计算。未知标准库异常向上传递。
     */
    [[nodiscard]] base::Result<TokenSequence> tokenize(
        std::string_view input, const TokenizationLimits& limits) const override;
};

} // namespace siftwing::text
