/*
 * PROJECT : SIFTWING
 * FILE    : query_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 查询选项、词频、错误传播、预算及拥有生命的行为验收
 */
#include "siftwing/query/query.h"
#include "siftwing/text/english_tokenizer.h"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) { throw std::runtime_error(label); }
}
template <typename T> void error(const base::Result<T> &result, const char *context) {
    check(!result && result.error().context == context, context);
}
query::QueryLimits limits() {
    return {{{1000,1000},{1000,100,100,1000},{100,1000,100,100,1000,100,1000}},100};
}
std::unique_ptr<text::TextPipeline> english() {
    return text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
        text::StopWords::parse({"the\n"},{100,10,10,100}).value()).value();
}
// 派生分词器故障只用于证明未调用/错误原样传递；业务词频另用真实英文管线及手写黄金。
class ControlledTokenizer final : public text::Tokenizer {
public:
    mutable unsigned calls = 0;
    bool throws = false;
    base::Result<text::TokenSequence> tokenize(std::string_view,
                                             const text::TokenizationLimits &) const override {
        ++calls;
        if (throws) { throw std::runtime_error("tokenizer-exception"); }
        return base::Result<text::TokenSequence>::failure({"original diagnostic", "test.tokenizer"});
    }
};
} // namespace

int main() {
    try {
        using namespace query;
        const auto p = english();
        const auto budget = limits();
        const auto normal = Query::from_text("THE Red red BLUE 17 red", *p, {}, budget).value();
        check(normal.mode() == QueryMode::any && normal.top_k() == 10, "default OR and ten results");
        check(!normal.empty() && normal.frequencies().total_tokens == 5 &&
            normal.frequencies().terms == text::FrequencyTable{{"17",1},{"blue",1},{"red",3}},
            "hand-written filtered TF includes repeats and sorted unique keys");
        const auto all = Query::from_text("RED blue", *p, {QueryMode::all,1}, budget).value();
        check(all.mode() == QueryMode::all && all.top_k() == 1, "explicit AND and lower endpoint");
        check(Query::from_text("red",*p,{QueryMode::any,100},budget).value().top_k() == 100,
            "inclusive upper endpoint");
        auto maximum = budget;
        maximum.max_top_k = std::numeric_limits<std::uint64_t>::max();
        check(Query::from_text("red",*p,{QueryMode::all,maximum.max_top_k},maximum).value().top_k()
            == maximum.max_top_k, "uint64 maximum is an ordinary configured endpoint");
        for (auto mode : {QueryMode::all,QueryMode::any}) {
            for (std::string_view input : {"", " \t\r\n", "...?!", "THE the", "中文😀"}) {
                error(Query::from_text(input,*p,{mode,1},budget),"query.empty");
            }
            const auto unknown = Query::from_text("unknown unknown",*p,{mode,1},budget).value();
            check(unknown.frequencies().terms
                == text::FrequencyTable{{"unknown",2}}, "unknown words retained without snapshot lookup");
        }
        error(Query::from_text("",*p,{QueryMode::all,0},budget),"query.top_k");
        error(Query::from_text("a",*p,{QueryMode::any,101},budget),"query.top_k");
        auto no_default = budget; no_default.max_top_k = 9;
        error(Query::from_text("a",*p,{},no_default),"query.top_k");
        auto zero = budget; zero.max_top_k = 0;
        const auto invalid = static_cast<QueryMode>(99);
        error(Query::from_text("",*p,{invalid,0},zero),"query.limits");
        error(Query::from_text("",*p,{invalid,0},budget),"query.mode");
        const std::string malformed{"red\xff",4};
        error(Query::from_text(malformed,*p,{},budget),"normalize.utf8");
        error(Query::from_text(std::string{"a\0b",3},*p,{},budget),"normalize.nul");
        // 每个正配置都独立置零；文本配置归已有管线诊断，不重新包装成query.empty。
        for (std::size_t i = 0; i < 13; ++i) {
            auto b = budget;
            auto &n = b.text.normalization; auto &t = b.text.tokenization; auto &a = b.text.analysis;
            const std::array<std::uint64_t *,13> fields{{&n.max_input_bytes,&n.max_output_bytes,
                &t.max_input_bytes,&t.max_tokens,&t.max_token_bytes,&t.max_output_bytes,
                &a.max_input_tokens,&a.max_input_bytes,&a.max_token_bytes,&a.max_output_tokens,
                &a.max_output_bytes,&a.max_distinct_terms,&a.max_vocabulary_bytes}};
            *fields[i] = 0;
            error(Query::from_text("",*p,{},b),"pipeline.limits");
        }
        const QueryLimits exact{{{3,3},{3,2,1,2},{2,2,1,2,2,1,1}},10};
        const auto exact_query = Query::from_text("A A",*p,{},exact).value();
        check(exact_query.frequencies().terms
            == text::FrequencyTable{{"a",2}}, "all text budgets at inclusive endpoints");
        const std::array<const char *,8> expected{{"normalize.input_bytes","normalize.output_bytes",
            "tokenizer.tokens","tokenizer.output_bytes","frequency.output_tokens",
            "frequency.output_bytes","frequency.distinct_terms","frequency.vocabulary_bytes"}};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            auto b = budget;
            if (i == 0) { b.text.normalization.max_input_bytes = 2; }
            if (i == 1) { b.text.normalization.max_output_bytes = 2; }
            if (i == 2) { b.text.tokenization.max_tokens = 1; }
            if (i == 3) { b.text.tokenization.max_output_bytes = 1; }
            if (i == 4) { b.text.analysis.max_output_tokens = 1; }
            if (i == 5) { b.text.analysis.max_output_bytes = 1; }
            if (i == 6) { b.text.analysis.max_distinct_terms = 1; }
            if (i == 7) { b.text.analysis.max_vocabulary_bytes = 1; }
            error(Query::from_text("a b",*p,{},b),expected[i]);
        }
        auto owned = [] {
            auto pipeline = english(); std::string input = "RED red BLUE";
            auto created = Query::from_text(input,*pipeline,{},limits()).value();
            input.assign(100,'x'); pipeline.reset(); return created;
        }();
        check(owned.frequencies().terms == text::FrequencyTable{{"blue",1},{"red",2}},
            "query survives input mutation and pipeline destruction");
        const auto copied = owned;
        check(&copied.frequencies() != &owned.frequencies() &&
            copied.frequencies().terms == owned.frequencies().terms, "copy owns independent complete TF");
        const auto moved = std::move(owned);
        check(owned.empty() && owned.frequencies().total_tokens == 0 &&
            owned.top_k() == 10 && owned.mode() == QueryMode::any, "move source is consistent empty TF");
        check(moved.frequencies().terms == copied.frequencies().terms && !moved.empty(), "move preserves target");
        const auto moved_empty = std::move(owned);
        check(moved_empty.empty() && moved_empty.frequencies().total_tokens == 0, "moving empty source remains observable");

        auto controlled = std::make_unique<ControlledTokenizer>();
        auto *observer = controlled.get();
        const auto failing = text::TextPipeline::create(std::move(controlled),
            text::StopWords::parse({}, {1,1,1,1}).value()).value();
        error(Query::from_text("a",*failing,{invalid,1},budget),"query.mode");
        error(Query::from_text("a",*failing,{QueryMode::any,0},budget),"query.top_k");
        check(observer->calls == 0, "invalid options fail before tokenizer execution");
        const auto propagated = Query::from_text("a",*failing,{},budget);
        check(!propagated && propagated.error().context == "test.tokenizer" &&
            propagated.error().message == "original diagnostic", "both diagnostic fields propagate unchanged");
        observer->throws = true;
        bool caught = false;
        try { (void)Query::from_text("a",*failing,{},budget); }
        catch (const std::runtime_error &e) { caught = std::string_view{e.what()} == "tokenizer-exception"; }
        check(caught, "unexpected tokenizer exception is not converted to ordinary query failure");
        const auto recovered = Query::from_text("red",*p,{},budget).value();
        check(recovered.frequencies().total_tokens == 1,
            "independent calls remain usable after rejected inputs");
        std::cout << "query checks=" << checks << " failures=0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "check " << checks << ": " << e.what() << '\n'; return 1;
    }
}
