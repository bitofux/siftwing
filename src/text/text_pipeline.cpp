/*
 * PROJECT : SIFTWING
 * FILE    : text_pipeline.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 组合既有文本阶段，固定顺序并保留失败诊断和拥有型结果
 */
#include "siftwing/text/text_pipeline.h"

#include <utility>

namespace siftwing::text {

TextPipeline::TextPipeline(std::unique_ptr<Tokenizer> tokenizer, StopWords stopwords)
    : tokenizer_(std::move(tokenizer)), stopwords_(std::move(stopwords)) {}

base::Result<std::unique_ptr<TextPipeline>>
TextPipeline::create(std::unique_ptr<Tokenizer> tokenizer, StopWords stopwords) {
    if (!tokenizer) {
        return base::Result<std::unique_ptr<TextPipeline>>::failure(
            {"分词器不能为空", "pipeline.tokenizer"});
    }
    // 私有构造不能由make_unique访问；new由表达式求值时的unique_ptr立即接管，异常不泄漏。
    return base::Result<std::unique_ptr<TextPipeline>>::success(
        std::unique_ptr<TextPipeline>(new TextPipeline(std::move(tokenizer), std::move(stopwords))));
}

base::Result<TokenAnalysis> TextPipeline::analyze(std::string_view input,
                                                const TextPipelineLimits &limits) const {
    const auto &n = limits.normalization;
    const auto &t = limits.tokenization;
    const auto &a = limits.analysis;
    // 先拒绝整份无效配置，避免早期输入失败或空内容掩盖下游零预算。
    if (n.max_input_bytes == 0 || n.max_output_bytes == 0 || t.max_input_bytes == 0 ||
        t.max_tokens == 0 || t.max_token_bytes == 0 || t.max_output_bytes == 0 ||
        a.max_input_tokens == 0 || a.max_input_bytes == 0 || a.max_token_bytes == 0 ||
        a.max_output_tokens == 0 || a.max_output_bytes == 0 || a.max_distinct_terms == 0 ||
        a.max_vocabulary_bytes == 0) {
        return base::Result<TokenAnalysis>::failure({"全部阶段预算必须为正数", "pipeline.limits"});
    }
    auto normalized = normalize_utf8(input, n);
    if (!normalized) {
        return base::Result<TokenAnalysis>::failure(std::move(normalized).error());
    }
    // normalized在同步tokenize期间保持存活；不持有跨调用string_view或保存缓存。
    auto tokens = tokenizer_->tokenize(normalized.value(), t);
    if (!tokens) {
        return base::Result<TokenAnalysis>::failure(std::move(tokens).error());
    }
    return analyze_tokens(tokens.value(), stopwords_, a);
}

} // namespace siftwing::text
