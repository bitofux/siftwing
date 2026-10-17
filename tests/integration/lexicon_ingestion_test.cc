/*
 * PROJECT : SIFTWING
 * FILE    : lexicon_ingestion_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 原创重复正文经共享管线分别进入推荐累计与搜索保留词表，核统计口径
 */
#include "siftwing/index/lexicon.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include "siftwing/text/text_pipeline.hpp"
#include "siftwing/document/duplicate_decision.hpp"
#include <iostream>

int main() {
    try {
        using namespace siftwing;
        auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({"the\n"}, {100, 10, 10, 100}).value())
                            .value();
        const text::TextPipelineLimits limits{
            {1000, 1000}, {1000, 100, 100, 1000}, {100, 1000, 100, 100, 1000, 100, 1000}};
        const auto first = pipeline->analyze("THE red RED blue", limits).value();
        const auto second = pipeline->analyze("the red red BLUE", limits).value();
        const auto empty = pipeline->analyze("the", limits).value();
        const text::RecommendationLimits rlimits{10, 100, 100, 1000, 100};
        // 推荐按显式交付累计两份重复正文和一份空正文；不把搜索去重报告隐式用作训练集。
        const auto r1 = text::accumulate_recommendation({}, first.tf, rlimits).value();
        const auto r2 = text::accumulate_recommendation(r1, second.tf, rlimits).value();
        const auto r3 = text::accumulate_recommendation(r2, empty.tf, rlimits).value();
        const index::LexiconLimits ll{100, 1000, 100, 100, 1000};
        const auto recommendation =
            index::RecommendationDictionary::build(r3, {ll, 10, 100, 100, 100}).value();
        const auto did = [](std::uint64_t n) {
            return base::DocumentId::from_integer(n).value();
        };
        const document::SimHashLimits sl{100, 100, 1000, 100};
        const std::vector<document::DuplicateCandidate> candidates{
            {did(9), {"original-a.txt", 0}, document::make_simhash(first.tf, sl).value()},
            {did(0), {"original-b.txt", 0}, document::make_simhash(second.tf, sl).value()}};
        const auto report =
            document::decide_duplicates(candidates, {}, {10, 100, 1000, 100}).value();
        std::vector<std::string_view> keys;
        for (const auto &entry : first.tf.terms) {
            keys.push_back(entry.first);
        }
        const auto search = index::Lexicon::build(keys, ll).value();
        const auto query = pipeline->analyze("RED", limits).value();
        unsigned checks = 0, failures = 0;
        const auto check = [&](bool v) {
            ++checks;
            if (!v) {
                ++failures;
            }
        };
        check(report.counts.kept == 1 && report.counts.duplicates == 1);
        check(first.tf.terms.at("red") == 2 && second.tf.terms.at("red") == 2);
        check(recommendation.documents() == 3 && recommendation.total_tokens() == 6);
        const auto red = recommendation.lexicon().find("red");
        check(red && recommendation.frequency(*red) == 4);
        check(search.find(query.tf.terms.begin()->first) == search.find("red"));
        check(search.terms() == std::vector<std::string>{"blue", "red"});
        check(recommendation.characters().at(U'r') == std::vector<base::TermId>{*red});
        check(first.tokens == std::vector<std::string>{"red", "red", "blue"});
        std::cout << "checks=" << checks << " failures=" << failures
                  << "; explicit recommendation deliveries and search kept vocabulary\n";
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
