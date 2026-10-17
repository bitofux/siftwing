/*
 * PROJECT : SIFTWING
 * FILE    : inverted_index_ingestion_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 共享管线到去重保留集再到搜索倒排，推荐仍显式累计重复正文
 */
#include "siftwing/index/inverted_index.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include "siftwing/text/text_pipeline.hpp"
#include "siftwing/document/duplicate_decision.hpp"
#include <iostream>

int main() {
    try {
        using namespace siftwing;
        const auto did = [](std::uint64_t n) {
            return base::DocumentId::from_integer(n).value();
        };
        auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({"the\n"}, {100, 10, 10, 100}).value())
                            .value();
        const text::TextPipelineLimits limits{
            {1000, 1000}, {1000, 100, 100, 1000}, {100, 1000, 100, 100, 1000, 100, 1000}};
        const auto a = pipeline->analyze("THE red RED blue", limits).value();
        const auto b = pipeline->analyze("the red red BLUE", limits).value();
        const auto e = pipeline->analyze("the", limits).value();
        const document::SimHashLimits sl{100, 100, 1000, 100};
        const std::vector<document::DuplicateCandidate> candidates{
            {did(9), {"a.txt", 0}, document::make_simhash(a.tf, sl).value()},
            {did(0), {"b.txt", 0}, document::make_simhash(b.tf, sl).value()},
            {did(42), {"empty.txt", 0}, document::make_simhash(e.tf, sl).value()}};
        const auto report =
            document::decide_duplicates(candidates, {}, {10, 100, 1000, 100}).value();
        const std::vector<index::IndexDocument> all{
            {did(9), a.tf}, {did(0), b.tf}, {did(42), e.tf}};
        // 根据真实报告逐篇选择保留者；输入倒排的是单文档TF，推荐使用所有显式交付。
        std::vector<index::IndexDocument> selected;
        for (const auto &decision : report.decisions) {
            if (decision.disposition != document::DuplicateDisposition::kept) {
                continue;
            }
            for (const auto &entry : all) {
                if (entry.document_id == decision.doc_id) {
                    selected.push_back(entry);
                }
            }
        }
        // M1.16默认跳过无特征；本夹具额外显式选入空文档，验证构建器不重做上游决策。
        selected.push_back({did(42), e.tf});
        const auto search =
            index::InvertedIndex::build(selected, {{100, 1000, 100, 100, 1000}, 10, 100, 1000, 10})
                .value();
        const text::RecommendationLimits rl{10, 100, 100, 1000, 100};
        const auto r1 = text::accumulate_recommendation({}, a.tf, rl).value();
        const auto r2 = text::accumulate_recommendation(r1, b.tf, rl).value();
        const auto rec = text::accumulate_recommendation(r2, e.tf, rl).value();
        unsigned checks = 0, failures = 0;
        const auto check = [&](bool ok) {
            ++checks;
            if (!ok) {
                ++failures;
            }
        };
        check(report.counts.kept == 1 && report.counts.duplicates == 1
              && report.counts.no_features == 1 && selected.size() == 2);
        check(search.statistics().documents == 2 && search.statistics().total_tokens == 3
              && search.statistics().total_postings == 2);
        const auto red = search.lexicon().find("red");
        check(red.has_value());
        if (red) {
            check(search.document_frequency(*red) == 1);
            const auto *p = search.postings(*red);
            check(p && p->size() == 1 && p->front().document_id() == did(9)
                  && p->front().term_frequency() == 2);
        }
        check(!search.document(did(0)) && search.document(did(42))
              && search.document(did(42))->total_tokens == 0);
        check(rec.documents == 3 && rec.total_tokens == 6 && rec.terms.at("red") == 4);
        check(search.lexicon().terms() == std::vector<std::string>{"blue", "red"});
        std::cout << "checks=" << checks << " failures=" << failures << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
