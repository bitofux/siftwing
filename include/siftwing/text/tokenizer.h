/*
 * PROJECT : SIFTWING
 * FILE    : tokenizer.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 定义建库和查询可共用的同步、拥有型分词接口
 */

#pragma once

#include "siftwing/base/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace siftwing::text {

/** @brief 拥有每个token的有序序列；保留重复，不附带词频、身份或源位置。 */
using TokenSequence = std::vector<std::string>;

/** @brief 调用方显式提供的四项正预算；按一次调用计，等于上限合法。 */
struct TokenizationLimits final {
    std::uint64_t max_input_bytes = 0; ///< 输入UTF-8字节；完整校验及输出分配前检查。
    std::uint64_t max_tokens = 0; ///< 输出token条数，重复词也分别计数。
    std::uint64_t max_token_bytes = 0; ///< 任一token的UTF-8字节数，不是码点数。
    std::uint64_t max_output_bytes = 0; ///< 所有token字节之和，不含分隔符或容器开销。
};

/**
 * @brief 共享分词边界；具体词边界、配置和并发能力由各实现进一步限定
 *
 * @note 返回token拥有存储，不依赖输入或Tokenizer存活。不隐式执行规范化、停用词过滤、
 *     词频统计或词项编号；调用方可以先调用normalize_utf8，再显式交付其拥有字符串。
 *     const接口本身不保证所有派生实现线程安全；当前结果的并发写仍由调用方同步。
 */
class Tokenizer {
public:
    /** @brief 多态销毁接口；派生资源须按其合同释放，不向析构调用方抛出异常。 */
    virtual ~Tokenizer() noexcept = default;

    /**
     * @brief 同步验证并生成拥有型token序列
     *
     * @param[in] input
     *     借用完整UTF-8字节；调用期间存活且无并发写入，非法编码和NUL须失败而不修复。
     * @param[in] limits
     *     借用四项正预算；先检查配置/输入字节，再完整编码/NUL，最后检查输出预算。
     *
     * @return
     *     success拥有全部token，空输入成功返回空序列；failure拥有诊断，无截短token或
     *     前缀序列。Error.context区分tokenizer.limits/input_bytes/utf8/nul/token_bytes/
     *     tokens/output_bytes；编码/NUL诊断含首个非法序列/NUL的0基字节位置。
     *
     * @note 此接口无业务IO；词边界及无词文本行为见具体实现。预算不等于整个进程的内存
     *     或CPU硬限，不包含调用方输入装载、vector容量及string对象/分配器开销。
     * @throws
     *     分配或其他未预期异常向上传递，不转换成成功或普通内容诊断。
     */
    [[nodiscard]] virtual base::Result<TokenSequence> tokenize(
        std::string_view input, const TokenizationLimits& limits) const = 0;
};

} // namespace siftwing::text
