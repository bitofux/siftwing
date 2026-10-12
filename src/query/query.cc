/*
 * PROJECT : SIFTWING
 * FILE    : query.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 选项检查与文本结果的查询业务转换
 * IMPLEMENTATION : 空文本在文本层合法，只有查询边界将空TF转为业务错误。
 */
#include "siftwing/query/query.hpp"

#include <type_traits>
#include <utility>

namespace siftwing::query {

Query::Query(text::DocumentTermFrequencies frequencies, QueryOptions options)
    : frequencies_(std::move(frequencies)), options_(options) {}

Query::Query(Query &&other) noexcept
    : frequencies_(std::move(other.frequencies_)), options_(other.options_) {
    static_assert(std::is_nothrow_move_constructible_v<text::DocumentTermFrequencies>);
    // 容器移动后内容未指定；同时清空词表/总数，避免移空源留下不一致统计。
    other.frequencies_.terms.clear();
    other.frequencies_.total_tokens = 0;
}

base::Result<Query> Query::from_text(std::string_view input, const text::TextPipeline &pipeline,
                                   QueryOptions options, const QueryLimits &limits) {
    if (limits.max_top_k == 0) {
        return base::Result<Query>::failure({"查询条数预算必须为正", "query.limits"});
    }
    if (options.mode != QueryMode::all && options.mode != QueryMode::any) {
        return base::Result<Query>::failure({"查询模式须为AND或OR", "query.mode"});
    }
    if (options.top_k == 0 || options.top_k > limits.max_top_k) {
        return base::Result<Query>::failure({"请求条数超出查询预算", "query.top_k"});
    }
    auto analysis = pipeline.analyze(input, limits.text);
    if (!analysis) {
        return base::Result<Query>::failure(std::move(analysis).error());
    }
    if (analysis.value().tf.terms.empty()) {
        return base::Result<Query>::failure({"查询没有有效词项", "query.empty"});
    }
    // 不复制保留token序列或原文；只转交后续词集合/向量所需的拥有TF。
    return base::Result<Query>::success(Query{std::move(analysis.value().tf), options});
}

const text::DocumentTermFrequencies &Query::frequencies() const & noexcept {
    return frequencies_;
}

} // namespace siftwing::query
