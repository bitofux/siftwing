/*
 * PROJECT : SIFTWING
 * FILE    : term_frequency.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 将停用词配置、受检TF及推荐累计隔离为确定性内存组件
 * IMPLEMENTATION :
 * -- 全编码校验先于输出预算，坏的停用token也会拒绝
 * -- set/map拥有词键，大小写/顺序由调用方显式规范化与分词决定
 * -- 推荐先验证输入再在新局部表合并，失败或分配异常都不改调用方统计
 */
#include "siftwing/text/term_frequency.hpp"
#include "siftwing/base/checked.hpp"
#include "siftwing/text/normalize.hpp"
#include <utf8.h>
namespace siftwing::text {
namespace {
template <class T>
base::Result<T> fail(std::string context, std::string message) {
    return base::Result<T>::failure({std::move(message), std::move(context)});
}
base::Result<void> problem(std::string context, std::string message) {
    return base::Result<void>::failure({std::move(message), std::move(context)});
}
bool whitespace(std::uint32_t cp) noexcept {
    return (cp >= 9U && cp <= 13U) || cp == 0x20U || cp == 0x85U || cp == 0xa0U || cp == 0x1680U
           || (cp >= 0x2000U && cp <= 0x200aU) || cp == 0x2028U || cp == 0x2029U || cp == 0x202fU
           || cp == 0x205fU || cp == 0x3000U;
}
bool has_space(std::string_view word) {
    auto current = word.begin();
    while (current != word.end()) {
        if (whitespace(utf8::next(current, word.end()))) {
            return true;
        }
    }
    return false;
}
base::Result<void> encoding(std::string_view bytes, const std::string &prefix, std::size_t index) {
    const auto invalid = utf8::find_invalid(bytes.begin(), bytes.end());
    if (invalid != bytes.end()) {
        return problem(prefix + ".utf8",
            "source/token " + std::to_string(index) + " invalid UTF-8 at byte "
                + std::to_string(invalid - bytes.begin()));
    }
    const auto nul = bytes.find('\0');
    if (nul != bytes.npos) {
        return problem(prefix + ".nul",
            "source/token " + std::to_string(index) + " NUL at byte " + std::to_string(nul));
    }
    return base::Result<void>::success();
}
// 以下表校验也用于外部可构造的统计值；不能假定公开字段总是本模块生成。
base::Result<std::uint64_t> validate_table(
    const FrequencyTable &table, std::uint64_t total, const RecommendationLimits &limits) {
    if (table.size() > limits.max_distinct_terms) {
        return fail<std::uint64_t>(
            "recommendation.distinct_terms", "table exceeds unique term budget");
    }
    for (const auto &entry : table) {
        auto valid = encoding(entry.first, "recommendation", 0);
        if (!valid) {
            return base::Result<std::uint64_t>::failure(std::move(valid).error());
        }
    }
    std::uint64_t sum = 0, bytes = 0;
    for (const auto &entry : table) {
        if (entry.first.empty() || has_space(entry.first)) {
            return fail<std::uint64_t>(
                "recommendation.term", "term must be nonempty without whitespace");
        }
        if (entry.second == 0) {
            return fail<std::uint64_t>(
                "recommendation.table", "table frequencies must be positive");
        }
        if (entry.first.size() > limits.max_term_bytes) {
            return fail<std::uint64_t>("recommendation.term_bytes", "term exceeds byte budget");
        }
        if (entry.first.size() > limits.max_vocabulary_bytes - bytes) {
            return fail<std::uint64_t>(
                "recommendation.vocabulary_bytes", "table keys exceed byte budget");
        }
        bytes += entry.first.size();
        auto next = base::checked_add(sum, entry.second, "recommendation.overflow");
        if (!next) {
            return base::Result<std::uint64_t>::failure(std::move(next).error());
        }
        sum = next.value();
    }
    if (sum != total) {
        return fail<std::uint64_t>("recommendation.table", "table sum differs from total_tokens");
    }
    if (total > limits.max_total_tokens) {
        return fail<std::uint64_t>(
            "recommendation.total_tokens", "table exceeds total occurrence budget");
    }
    return base::Result<std::uint64_t>::success(bytes);
}
bool recommendation_limits_valid(const RecommendationLimits &limits) noexcept {
    return limits.max_documents && limits.max_distinct_terms && limits.max_term_bytes
           && limits.max_vocabulary_bytes && limits.max_total_tokens;
}
struct AccumulationCounts {
    std::uint64_t documents;
    std::uint64_t total_tokens;
};
// 只验证外部交付及新计数；内部累计由本模块维护，不逐篇重扫其词表。
base::Result<AccumulationCounts> next_counts(const RecommendationFrequencies &current,
    const DocumentTermFrequencies &document, const RecommendationLimits &limits) {
    auto valid = validate_table(document.terms, document.total_tokens, limits);
    if (!valid) {
        return base::Result<AccumulationCounts>::failure(std::move(valid).error());
    }
    auto documents =
        base::checked_add(current.documents, std::uint64_t{1}, "recommendation.overflow");
    if (!documents) {
        return base::Result<AccumulationCounts>::failure(std::move(documents).error());
    }
    if (documents.value() > limits.max_documents) {
        return fail<AccumulationCounts>("recommendation.documents", "document count exceeds budget");
    }
    auto total =
        base::checked_add(current.total_tokens, document.total_tokens, "recommendation.overflow");
    if (!total) {
        return base::Result<AccumulationCounts>::failure(std::move(total).error());
    }
    if (total.value() > limits.max_total_tokens) {
        return fail<AccumulationCounts>(
            "recommendation.total_tokens", "cumulative occurrences exceed budget");
    }
    return base::Result<AccumulationCounts>::success({documents.value(), total.value()});
}
// 仅修改尚未发布的局部表；中途失败/异常时调用方销毁整表，不暴露部分累计。
base::Result<void> merge_document(RecommendationFrequencies &output,
    const DocumentTermFrequencies &document, AccumulationCounts counts,
    const RecommendationLimits &limits, std::uint64_t &bytes) {
    for (const auto &entry : document.terms) {
        auto found = output.terms.find(entry.first);
        if (found == output.terms.end()) {
            if (output.terms.size() == limits.max_distinct_terms) {
                return problem(
                    "recommendation.distinct_terms", "merged unique term count exceeds budget");
            }
            if (entry.first.size() > limits.max_vocabulary_bytes - bytes) {
                return problem(
                    "recommendation.vocabulary_bytes", "merged unique term bytes exceed budget");
            }
            bytes += entry.first.size();
            output.terms.insert(entry);
        } else {
            auto next = base::checked_add(found->second, entry.second, "recommendation.overflow");
            if (!next) {
                return base::Result<void>::failure(std::move(next).error());
            }
            found->second = next.value();
        }
    }
    output.documents = counts.documents;
    output.total_tokens = counts.total_tokens;
    return base::Result<void>::success();
}
} // namespace
base::Result<StopWords> StopWords::parse(
    const std::vector<std::string_view> &sources, const StopWordLimits &limits) {
    if (limits.max_input_bytes == 0 || limits.max_entries == 0 || limits.max_entry_bytes == 0
        || limits.max_vocabulary_bytes == 0) {
        return fail<StopWords>("stopwords.limits", "four positive configuration limits required");
    }
    std::uint64_t input_bytes = 0;
    for (const auto source : sources) {
        if (source.size() > limits.max_input_bytes - input_bytes) {
            return fail<StopWords>("stopwords.input_bytes", "source bytes exceed budget");
        }
        input_bytes += source.size();
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        auto valid = encoding(sources[i], "stopwords", i);
        if (!valid) {
            return base::Result<StopWords>::failure(std::move(valid).error());
        }
    }
    std::set<std::string, std::less<>> entries;
    std::uint64_t vocabulary_bytes = 0;
    for (auto source : sources) {
        if (source.substr(0, 3) == "\xef\xbb\xbf") {
            source.remove_prefix(3);
        }
        for (std::size_t begin = 0; begin < source.size();) {
            auto end = source.find('\n', begin);
            if (end == source.npos) {
                end = source.size();
            }
            const auto line = source.substr(begin, end - begin);
            begin = end + 1;
            const auto size =
                base::checked_narrow<std::uint64_t>(line.size(), "stopwords.input_bytes");
            if (!size) {
                return base::Result<StopWords>::failure(std::move(size).error());
            }
            if (line.empty()) {
                continue;
            }
            auto normalized = normalize_utf8(line, {size.value(), size.value()});
            if (!normalized) {
                return base::Result<StopWords>::failure(std::move(normalized).error());
            }
            auto entry = std::move(normalized).value();
            if (entry.empty()) {
                continue;
            }
            if (entry.find(' ') != entry.npos) {
                return fail<StopWords>("stopwords.entry", "one normalized entry per line required");
            }
            if (entry.size() > limits.max_entry_bytes) {
                return fail<StopWords>(
                    "stopwords.entry_bytes", "normalized entry exceeds byte budget");
            }
            if (entries.find(entry) != entries.end()) {
                continue;
            }
            if (entries.size() == limits.max_entries) {
                return fail<StopWords>("stopwords.entries", "unique entry count exceeds budget");
            }
            if (entry.size() > limits.max_vocabulary_bytes - vocabulary_bytes) {
                return fail<StopWords>(
                    "stopwords.vocabulary_bytes", "unique entry bytes exceed budget");
            }
            vocabulary_bytes += entry.size();
            entries.insert(std::move(entry));
        }
    }
    return base::Result<StopWords>::success(StopWords(std::move(entries)));
}
bool StopWords::contains(std::string_view token) const {
    return entries_.find(token) != entries_.end();
}
base::Result<TokenAnalysis> analyze_tokens(
    const TokenSequence &input, const StopWords &stopwords, const TokenAnalysisLimits &limits) {
    if (limits.max_input_tokens == 0 || limits.max_input_bytes == 0 || limits.max_token_bytes == 0
        || limits.max_output_tokens == 0 || limits.max_output_bytes == 0
        || limits.max_distinct_terms == 0 || limits.max_vocabulary_bytes == 0) {
        return fail<TokenAnalysis>("frequency.limits", "seven positive analysis limits required");
    }
    if (input.size() > limits.max_input_tokens) {
        return fail<TokenAnalysis>("frequency.input_tokens", "input token count exceeds budget");
    }
    std::uint64_t input_bytes = 0;
    for (const auto &token : input) {
        if (token.size() > limits.max_input_bytes - input_bytes) {
            return fail<TokenAnalysis>("frequency.input_bytes", "input token bytes exceed budget");
        }
        input_bytes += token.size();
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        auto valid = encoding(input[i], "frequency", i);
        if (!valid) {
            return base::Result<TokenAnalysis>::failure(std::move(valid).error());
        }
    }
    for (const auto &token : input) {
        if (token.empty() || has_space(token)) {
            return fail<TokenAnalysis>(
                "frequency.token", "token must be nonempty without whitespace");
        }
        if (token.size() > limits.max_token_bytes) {
            return fail<TokenAnalysis>("frequency.token_bytes", "input token exceeds byte budget");
        }
    }
    TokenAnalysis output;
    std::uint64_t bytes = 0, vocabulary_bytes = 0;
    for (const auto &token : input) {
        if (stopwords.contains(token)) {
            continue;
        }
        if (output.tokens.size() == limits.max_output_tokens) {
            return fail<TokenAnalysis>(
                "frequency.output_tokens", "retained token count exceeds budget");
        }
        if (token.size() > limits.max_output_bytes - bytes) {
            return fail<TokenAnalysis>(
                "frequency.output_bytes", "retained token bytes exceed budget");
        }
        bytes += token.size();
        auto found = output.tf.terms.find(token);
        if (found == output.tf.terms.end()) {
            if (output.tf.terms.size() == limits.max_distinct_terms) {
                return fail<TokenAnalysis>(
                    "frequency.distinct_terms", "unique term count exceeds budget");
            }
            if (token.size() > limits.max_vocabulary_bytes - vocabulary_bytes) {
                return fail<TokenAnalysis>(
                    "frequency.vocabulary_bytes", "unique term bytes exceed budget");
            }
            vocabulary_bytes += token.size();
            output.tf.terms.emplace(token, 1);
        } else {
            auto next = base::checked_add(found->second, std::uint64_t{1}, "frequency.overflow");
            if (!next) {
                return base::Result<TokenAnalysis>::failure(std::move(next).error());
            }
            found->second = next.value();
        }
        auto total =
            base::checked_add(output.tf.total_tokens, std::uint64_t{1}, "frequency.overflow");
        if (!total) {
            return base::Result<TokenAnalysis>::failure(std::move(total).error());
        }
        output.tf.total_tokens = total.value();
        output.tokens.push_back(token);
    }
    return base::Result<TokenAnalysis>::success(std::move(output));
}
base::Result<RecommendationFrequencies> accumulate_recommendation(
    const RecommendationFrequencies &current,
    const DocumentTermFrequencies &document,
    const RecommendationLimits &limits) {
    if (!recommendation_limits_valid(limits)) {
        return fail<RecommendationFrequencies>(
            "recommendation.limits", "five positive accumulation limits required");
    }
    auto current_valid = validate_table(current.terms, current.total_tokens, limits);
    if (!current_valid) {
        return base::Result<RecommendationFrequencies>::failure(std::move(current_valid).error());
    }
    if (current.documents == 0 && !current.terms.empty()) {
        return fail<RecommendationFrequencies>(
            "recommendation.table", "nonempty corpus requires a document count");
    }
    if (current.documents > limits.max_documents) {
        return fail<RecommendationFrequencies>(
            "recommendation.documents", "existing document count exceeds budget");
    }
    auto counts = next_counts(current, document, limits);
    if (!counts) {
        return base::Result<RecommendationFrequencies>::failure(std::move(counts).error());
    }
    RecommendationFrequencies output = current;
    auto bytes = current_valid.value();
    auto merged = merge_document(output, document, counts.value(), limits, bytes);
    if (!merged) {
        return base::Result<RecommendationFrequencies>::failure(std::move(merged).error());
    }
    return base::Result<RecommendationFrequencies>::success(std::move(output));
}
base::Result<RecommendationFrequencies> accumulate_recommendation_batch(
    const std::vector<DocumentTermFrequencies> &documents,
    const RecommendationLimits &limits) {
    if (!recommendation_limits_valid(limits)) {
        return fail<RecommendationFrequencies>(
            "recommendation.limits", "five positive accumulation limits required");
    }
    RecommendationFrequencies output;
    std::uint64_t bytes = 0;
    for (const auto &document : documents) {
        auto counts = next_counts(output, document, limits);
        if (!counts) {
            return base::Result<RecommendationFrequencies>::failure(std::move(counts).error());
        }
        auto merged = merge_document(output, document, counts.value(), limits, bytes);
        if (!merged) {
            return base::Result<RecommendationFrequencies>::failure(std::move(merged).error());
        }
    }
    return base::Result<RecommendationFrequencies>::success(std::move(output));
}
} // namespace siftwing::text
