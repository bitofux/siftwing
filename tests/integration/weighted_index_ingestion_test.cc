/*
 * PROJECT : SIFTWING
 * FILE    : weighted_index_ingestion_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 共享管线/去重保留/原始倒排到单位权重，推荐累计仍独立
 */
#include "siftwing/index/weighted_index.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include "siftwing/text/text_pipeline.hpp"
#include "siftwing/document/duplicate_decision.hpp"
#include <cmath>
#include <iostream>

int main() {
    try {
        using namespace siftwing;
        auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({"the\n"}, {100, 10, 10, 100}).value())
                            .value();
        const text::TextPipelineLimits limits{
            {1000, 1000}, {1000, 100, 100, 1000}, {100, 1000, 100, 100, 1000, 100, 1000}};
        const auto a = pipeline->analyze("THE red RED blue", limits).value(),
                   b = pipeline->analyze("the red red BLUE", limits).value();
        const auto empty = pipeline->analyze("the", limits).value();
        const auto did = [](std::uint64_t n) {
            return base::DocumentId::from_integer(n).value();
        };
        const document::SimHashLimits sl{100, 100, 1000, 100};
        const std::vector<document::DuplicateCandidate> candidates{
            {did(9), {"a.txt", 0}, document::make_simhash(a.tf, sl).value()},
            {did(0), {"b.txt", 0}, document::make_simhash(b.tf, sl).value()}};
        const auto report =
            document::decide_duplicates(candidates, {}, {10, 100, 1000, 100}).value();
        std::vector<index::IndexDocument> selected;
        for (const auto &decision : report.decisions) {
            if (decision.disposition == document::DuplicateDisposition::kept) {
                selected.push_back({decision.doc_id, decision.doc_id == did(9) ? a.tf : b.tf});
            }
        }
        // 调用方额外显式选入空篇；不更改M1.16无特征默认跳过的规则。
        selected.push_back({did(42), empty.tf});
        auto raw =
            index::InvertedIndex::build(selected, {{100, 1000, 100, 100, 1000}, 10, 100, 1000, 10})
                .value();
        const auto weighted = index::WeightedIndex::build(std::move(raw), {10, 100, 100}).value();
        const text::RecommendationLimits rl{10, 100, 100, 1000, 100};
        const auto first = text::accumulate_recommendation({}, a.tf, rl).value();
        const auto rec = text::accumulate_recommendation(first, b.tf, rl).value();
        unsigned checks = 0, failures = 0;
        const auto check = [&](bool ok) {
            ++checks;
            if (!ok) {
                ++failures;
            }
        };
        check(report.counts.kept == 1 && report.counts.duplicates == 1);
        check(weighted.raw_index().statistics().documents == 2
              && weighted.raw_index().statistics().total_tokens == 3);
        const auto red = *weighted.raw_index().lexicon().find("red"),
                   blue = *weighted.raw_index().lexicon().find("blue");
        check(weighted.raw_index().document_frequency(red) == 1
              && weighted.raw_index().postings(red)->front().term_frequency() == 2);
        check(std::abs(weighted.weights(red)->front() - 2.0 / std::sqrt(5.0)) < 1e-12
              && std::abs(weighted.weights(blue)->front() - 1.0 / std::sqrt(5.0)) < 1e-12);
        check(std::abs(*weighted.inverse_document_frequency(red) - (1 + std::log(3.0 / 2.0)))
              < 1e-12);
        check(weighted.document_norm(did(42)) == 0.0 && !weighted.document_norm(did(0)));
        check(rec.documents == 2 && rec.terms.at("red") == 4
              && weighted.raw_index().postings(red)->front().term_frequency() == 2);
        check(weighted.serialize({1000}).value().find("D 42 0 0 0000000000000000\n")
              != std::string::npos);
        std::cout << "checks=" << checks << " failures=" << failures << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
