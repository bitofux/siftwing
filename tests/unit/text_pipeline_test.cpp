/*
 * PROJECT : SIFTWING
 * FILE    : text_pipeline_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用手写黄金与分词替身验证阶段顺序、预算、拥有性和失败短路
 */
#include "siftwing/text/english_tokenizer.h"
#include "siftwing/text/text_pipeline.h"

#include <array>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace siftwing::text;
unsigned checks = 0, failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
template <typename T> void failure(const siftwing::base::Result<T> &result, const char *context) {
    check(!result, "expected failure");
    if (result) {
        return;
    }
    check(result.error().context == context, "failure keeps stage context");
    bool no_payload = false;
    try {
        (void)result.value();
    } catch (const std::bad_variant_access &) {
        no_payload = true;
    }
    check(no_payload, "failure publishes no partial result");
}
StopWords stops(std::string_view text = {}) {
    auto parsed = StopWords::parse({text}, {4096, 100, 100, 4096});
    if (!parsed) {
        throw std::runtime_error(parsed.error().context);
    }
    return parsed.value();
}
std::unique_ptr<TextPipeline> english(std::string_view text = {}) {
    auto created = TextPipeline::create(std::make_unique<EnglishTokenizer>(), stops(text));
    if (!created) {
        throw std::runtime_error(created.error().context);
    }
    return std::move(created).value();
}
const TextPipelineLimits broad{{4096, 4096}, {4096, 100, 100, 4096},
                               {100, 4096, 100, 100, 4096, 100, 4096}};
enum class Mode { normal, fail, invalid_utf8, empty_token, allocation, unexpected };
struct Observation {
    unsigned calls = 0, destroyed = 0;
    std::string input;
    TokenizationLimits limits;
};
class ObservingTokenizer final : public Tokenizer {
  public:
    ObservingTokenizer(Observation &observation, Mode mode) : observation_(observation), mode_(mode) {}
    ~ObservingTokenizer() noexcept override { ++observation_.destroyed; }
    siftwing::base::Result<TokenSequence> tokenize(std::string_view input,
                                                 const TokenizationLimits &limits) const override {
        ++observation_.calls;
        observation_.input = input;
        observation_.limits = limits;
        switch (mode_) {
        case Mode::fail:
            return siftwing::base::Result<TokenSequence>::failure({"原始诊断", "test.tokenizer"});
        case Mode::invalid_utf8:
            return siftwing::base::Result<TokenSequence>::success({std::string("\xff", 1)});
        case Mode::empty_token:
            return siftwing::base::Result<TokenSequence>::success({""});
        case Mode::allocation:
            throw std::bad_alloc();
        case Mode::unexpected:
            throw std::runtime_error("tokenizer exception");
        case Mode::normal:
            return EnglishTokenizer{}.tokenize(input, limits);
        }
        throw std::logic_error("unreachable mode");
    }
  private:
    Observation &observation_;
    Mode mode_;
};
} // namespace

int main() {
    static_assert(!std::is_default_constructible_v<TextPipeline>);
    static_assert(!std::is_copy_constructible_v<TextPipeline>);
    static_assert(!std::is_move_constructible_v<TextPipeline>);
    try {
        failure(TextPipeline::create({}, stops()), "pipeline.tokenizer");
        auto pipeline = english(" THE\r\nAND\n");
        const std::string raw = "　THE Red red\tAND blue42！ ";
        auto result = pipeline->analyze(raw, broad);
        const TokenSequence expected{"red", "red", "blue42"};
        const FrequencyTable tf{{"red", 2}, {"blue42", 1}};
        check(result && result.value().tokens == expected, "hand-written ordered filtered words");
        check(result && result.value().tf.terms == tf && result.value().tf.total_tokens == 3,
              "hand-written TF counts repeated red twice");
        check(raw == "　THE Red red\tAND blue42！ ", "borrowed input unchanged");
        auto empty = pipeline->analyze("", broad);
        auto whitespace = pipeline->analyze("\r\n　\t", broad);
        auto filtered = pipeline->analyze("THE and THE", broad);
        auto delimiters = pipeline->analyze("！！！中文", broad);
        for (const auto *value : {&empty, &whitespace, &filtered, &delimiters}) {
            check(*value && value->value().tokens.empty() && value->value().tf.terms.empty() &&
                      value->value().tf.total_tokens == 0,
                  "empty no-word and all-filtered inputs are successful empty analyses");
        }
        auto edges = pipeline->analyze("THE Café ΑΒ 中文 a1 DON'T foo_bar 3.14", broad);
        check(edges && edges.value().tokens == TokenSequence{"caf", "a1", "don", "t", "foo",
                                                               "bar", "3", "14"},
              "pipeline preserves existing English boundaries after ASCII normalization");
        // 手写8输入字节、3条token/6词字节、2唯一词/4键字节；等于各阶段上限合法。
        auto plain = english();
        const TextPipelineLimits exact{{8, 8}, {8, 3, 2, 6}, {3, 6, 2, 3, 6, 2, 4}};
        auto boundary = plain->analyze("AA BB AA", exact);
        check(boundary && boundary.value().tokens == TokenSequence{"aa", "bb", "aa"} &&
                  boundary.value().tf.terms == FrequencyTable{{"aa", 2}, {"bb", 1}},
              "all exact stage budgets succeed");
        const std::array<std::uint64_t NormalizationLimits::*, 2> nm{
            &NormalizationLimits::max_input_bytes, &NormalizationLimits::max_output_bytes};
        const std::array<const char *, 2> nc{"normalize.input_bytes", "normalize.output_bytes"};
        for (std::size_t i = 0; i < nm.size(); ++i) {
            auto limit = exact;
            --(limit.normalization.*nm[i]);
            failure(plain->analyze("AA BB AA", limit), nc[i]);
        }
        const std::array<std::uint64_t TokenizationLimits::*, 4> tm{
            &TokenizationLimits::max_input_bytes, &TokenizationLimits::max_tokens,
            &TokenizationLimits::max_token_bytes, &TokenizationLimits::max_output_bytes};
        const std::array<const char *, 4> tc{"tokenizer.input_bytes", "tokenizer.tokens",
                                             "tokenizer.token_bytes", "tokenizer.output_bytes"};
        for (std::size_t i = 0; i < tm.size(); ++i) {
            auto limit = exact;
            --(limit.tokenization.*tm[i]);
            failure(plain->analyze("AA BB AA", limit), tc[i]);
        }
        const std::array<std::uint64_t TokenAnalysisLimits::*, 7> am{
            &TokenAnalysisLimits::max_input_tokens, &TokenAnalysisLimits::max_input_bytes,
            &TokenAnalysisLimits::max_token_bytes, &TokenAnalysisLimits::max_output_tokens,
            &TokenAnalysisLimits::max_output_bytes, &TokenAnalysisLimits::max_distinct_terms,
            &TokenAnalysisLimits::max_vocabulary_bytes};
        const std::array<const char *, 7> ac{
            "frequency.input_tokens", "frequency.input_bytes", "frequency.token_bytes",
            "frequency.output_tokens", "frequency.output_bytes", "frequency.distinct_terms",
            "frequency.vocabulary_bytes"};
        for (std::size_t i = 0; i < am.size(); ++i) {
            auto limit = exact;
            --(limit.analysis.*am[i]);
            failure(plain->analyze("AA BB AA", limit), ac[i]);
        }
        // 观察下游未被调用；任何阶段零配置都先于内容错误/空输入被拒绝。
        Observation observation;
        auto observing = std::move(TextPipeline::create(
            std::make_unique<ObservingTokenizer>(observation, Mode::normal), stops())).value();
        for (const auto member : nm) {
            auto limit = broad;
            limit.normalization.*member = 0;
            failure(observing->analyze("", limit), "pipeline.limits");
        }
        for (const auto member : tm) {
            auto limit = broad;
            limit.tokenization.*member = 0;
            failure(observing->analyze("\xff", limit), "pipeline.limits");
        }
        for (const auto member : am) {
            auto limit = broad;
            limit.analysis.*member = 0;
            failure(observing->analyze("", limit), "pipeline.limits");
        }
        check(observation.calls == 0, "all 13 invalid budgets reject before calling tokenizer");
        failure(observing->analyze("\xff", broad), "normalize.utf8");
        failure(observing->analyze(std::string("a\0b", 3), broad), "normalize.nul");
        auto little = broad;
        little.normalization.max_input_bytes = 1;
        failure(observing->analyze("AB", little), "normalize.input_bytes");
        check(observation.calls == 0, "normalization failures short-circuit downstream tokenizer");
        auto seen = observing->analyze("  A\tＢ　C  ", broad);
        check(seen && observation.calls == 1 && observation.input == "a Ｂ c",
              "tokenizer sees normalized UTF-8 once, no NFKC normalization");
        check(observation.limits.max_input_bytes == broad.tokenization.max_input_bytes &&
                  observation.limits.max_tokens == broad.tokenization.max_tokens &&
                  observation.limits.max_token_bytes == broad.tokenization.max_token_bytes &&
                  observation.limits.max_output_bytes == broad.tokenization.max_output_bytes,
              "tokenizer receives the caller's stage budgets unchanged");
        auto inherited = normalize_utf8("\xff", broad.normalization);
        auto forwarded = observing->analyze("\xff", broad);
        check(!inherited && !forwarded && inherited.error().message == forwarded.error().message &&
                  inherited.error().context == forwarded.error().context,
              "normalization diagnostics forwarded without wrapping");
        for (const Mode mode : {Mode::fail, Mode::invalid_utf8, Mode::empty_token}) {
            Observation bad;
            auto subject = std::move(TextPipeline::create(
                std::make_unique<ObservingTokenizer>(bad, mode), stops())).value();
            auto value = subject->analyze("text", broad);
            failure(value, mode == Mode::fail ? "test.tokenizer" :
                           mode == Mode::invalid_utf8 ? "frequency.utf8" : "frequency.token");
            check(bad.calls == 1, "one virtual call, no retry on stage failure");
            if (mode == Mode::fail) {
                check(value.error().message == "原始诊断", "custom tokenizer diagnostic unchanged");
            }
        }
        for (const Mode mode : {Mode::allocation, Mode::unexpected}) {
            Observation throwing;
            auto subject = std::move(TextPipeline::create(
                std::make_unique<ObservingTokenizer>(throwing, mode), stops())).value();
            bool propagated = false;
            try {
                (void)subject->analyze("text", broad);
            } catch (const std::bad_alloc &) {
                propagated = mode == Mode::allocation;
            } catch (const std::runtime_error &error) {
                propagated = mode == Mode::unexpected && std::string(error.what()) == "tokenizer exception";
            }
            check(propagated, "allocation and unknown tokenizer exceptions propagate");
            subject.reset();
            check(throwing.destroyed == 1, "throwing tokenizer destroyed exactly once by pipeline");
        }
        // 原文和管线先销毁，输出序列及TF仍独立拥有；多态析构不依赖借用输入。
        std::string temporary = "Keep KEEP";
        auto kept = observing->analyze(temporary, broad);
        temporary.clear();
        observing.reset();
        pipeline.reset();
        check(observation.destroyed == 1 && kept && kept.value().tokens == TokenSequence{"keep", "keep"} &&
                  kept.value().tf.terms.at("keep") == 2,
              "analysis owns tokens and TF beyond pipeline and input destruction");
        check(result.value().tokens == expected && result.value().tf.terms == tf,
              "earlier analysis survives unrelated calls and pipeline destruction");
        auto narrower = broad;
        narrower.normalization = {100, 100};
        narrower.tokenization = {100, 10, 10, 100};
        narrower.analysis = {10, 100, 10, 10, 100, 10, 100};
        auto one = plain->analyze("AA BB AA", broad);
        auto two = plain->analyze("AA BB AA", narrower);
        check(one && two && one.value().tokens == two.value().tokens &&
                  one.value().tf.terms == two.value().tf.terms,
              "different sufficient document and query budgets preserve term semantics");
        // 有效小预算仍按阶段计量：规范化后而非原文的字节供分词预算检查。
        auto stage = broad;
        stage.tokenization.max_input_bytes = 1;
        auto trimmed = plain->analyze("  A  ", stage);
        check(trimmed && trimmed.value().tokens == TokenSequence{"a"},
              "tokenizer input bytes count normalized text rather than raw source");
        auto no_stops = plain->analyze("the", broad);
        auto with_stops = english("the\n")->analyze("the", broad);
        check(no_stops && with_stops && no_stops.value().tokens.size() == 1 &&
                  with_stops.value().tokens.empty(),
              "different explicit stopword configurations can change terms");
    } catch (const std::exception &error) {
        ++failures;
        std::cerr << "unexpected: " << error.what() << '\n';
    }
    std::cout << "text pipeline checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
