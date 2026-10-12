/*
 * PROJECT : SIFTWING
 * FILE    : summary_queries.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 摘要测试使用显式竖线词键，避免将分词器的词边界误当摘要合同
 */
#pragma once
#include "siftwing/query/query.hpp"
#include <stdexcept>

namespace siftwing::test {
// 仅测试分词器：经过真实TextPipeline/Query验证，|分隔可手写的中文/emoji词键。
class SummaryTokenizer final : public text::Tokenizer {
public:
    base::Result<text::TokenSequence> tokenize(std::string_view input,
        const text::TokenizationLimits &limits) const override {
        using Output = base::Result<text::TokenSequence>;
        if (limits.max_input_bytes == 0 || limits.max_tokens == 0 ||
            limits.max_token_bytes == 0 || limits.max_output_bytes == 0) {
            return Output::failure({"测试分词预算须正", "tokenizer.limits"});
        }
        auto valid = text::normalize_utf8(input, {limits.max_input_bytes, limits.max_input_bytes});
        if (!valid) { return Output::failure(std::move(valid).error()); }
        text::TokenSequence tokens;
        std::uint64_t bytes = 0;
        std::size_t start = 0;
        while (start < input.size()) {
            const auto bar = input.find('|', start);
            const auto end = bar == input.npos ? input.size() : bar;
            if (end > start) {
                const auto word = input.substr(start, end - start);
                if (word.size() > limits.max_token_bytes || word.size() > limits.max_output_bytes - bytes ||
                    tokens.size() >= limits.max_tokens) {
                    return Output::failure({"测试词键超限", "tokenizer.tokens"});
                }
                tokens.emplace_back(word);
                bytes += static_cast<std::uint64_t>(word.size());
            }
            if (bar == input.npos) { break; }
            start = bar + 1;
        }
        return Output::success(std::move(tokens));
    }
};
inline std::unique_ptr<text::TextPipeline> summary_pipeline() {
    return text::TextPipeline::create(std::make_unique<SummaryTokenizer>(),
        text::StopWords::parse({}, {100, 10, 10, 100}).value()).value();
}
inline query::Query summary_query(std::string_view words, std::uint64_t k = 1,
                                 query::QueryMode mode = query::QueryMode::any) {
    auto pipeline = summary_pipeline();
    const query::QueryLimits limits{{{100000,100000},{100000,1000,10000,100000},
        {1000,100000,10000,1000,100000,1000,100000}},100};
    return query::Query::from_text(words, *pipeline, {mode,k}, limits).value();
}
} // namespace siftwing::test
