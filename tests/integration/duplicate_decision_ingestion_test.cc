/*
 * PROJECT : SIFTWING
 * FILE    : duplicate_decision_ingestion_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 原创TXT/RSS经读取、共享管线、SimHash后确定搜索保留集合
 */
#include "../support/cppjieba_resources.hpp"
#include "siftwing/document/duplicate_decision.hpp"
#include "siftwing/document/rss_reader.hpp"
#include "siftwing/document/txt_reader.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include "siftwing/text/text_pipeline.hpp"

#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0, failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
} // namespace

int main() {
    try {
        using namespace siftwing::document;
        using namespace siftwing::text;
        const auto id = [](std::uint64_t value) {
            return siftwing::base::DocumentId::from_integer(value).value();
        };
        const std::string raw = "THE red RED red blue42";
        const std::string xml =
            "<rss version='2.0'><channel><item><title>原创一</title>"
            "<description><![CDATA[<p>THE red RED red blue42</p>]]></description></item>"
            "<item><title>原创二</title><description>red red red green</description></item>"
            "<item><title>原创三</title><description>THE the</description></item>"
            "</channel></rss>";
        TxtDocumentReader txt({{"a.txt", {id(80), "原创"}}});
        RssDocumentReader rss(
            {{"b.xml", {id(0), id(1), id(2)}}}, {1000, 64, {4096, 4096, 1000, 1000, 64}});
        const auto first =
            read_documents({{SourceKind::txt, "a.txt", raw}}, txt, {4096, 4096, 10, 10000}, {})
                .value();
        const auto second =
            read_documents({{SourceKind::rss, "b.xml", xml}}, rss, {4096, 4096, 10, 10000}, {})
                .value();
        if (first.items.size() != 1 || second.items.size() != 3) {
            throw std::runtime_error("adapter counts");
        }
        auto pipeline = TextPipeline::create(std::make_unique<EnglishTokenizer>(),
            StopWords::parse({"the\n"}, {100, 10, 10, 100}).value())
                            .value();
        const TextPipelineLimits text_limits{
            {4096, 4096}, {4096, 100, 100, 4096}, {100, 4096, 100, 100, 4096, 100, 4096}};
        const SimHashLimits hash_limits{100, 100, 4096, 100};
        std::vector<DuplicateCandidate> candidates;
        const auto add = [&](const ReadItem &item) {
            if (!item.record) {
                throw std::runtime_error("original record missing");
            }
            const auto &record = *item.record;
            const auto analysis = pipeline->analyze(record.content, text_limits).value();
            candidates.push_back({record.doc_id,
                record.source.source_id,
                make_simhash(analysis.tf, hash_limits).value()});
        };
        // 故意RSS先传，函数仍按来源a.txt优先；编号0和RSS标题均不决定保留者。
        for (const auto &item : second.items) {
            add(item);
        }
        add(first.items[0]);
        const auto report = decide_duplicates(candidates, {}, {10, 100, 1000, 100}).value();
        check(report.counts.input == 4 && report.counts.kept == 1 && report.counts.duplicates == 2
                  && report.counts.no_features == 1 && report.counts.comparisons == 2,
            "TXT RSS full pipeline produces audited retained collection");
        check(report.decisions[0].doc_id == id(80)
                  && report.decisions[0].disposition == DuplicateDisposition::kept,
            "earliest source wins across format and numeric identity");
        check(report.decisions[1].match && report.decisions[1].match->retained_id == id(80)
                  && report.decisions[1].match->distance == 0,
            "exact duplicate body points to TXT");
        check(report.decisions[2].match && report.decisions[2].match->retained_id == id(80)
                  && report.decisions[2].match->distance == 0,
            "minority-word change sample meets policy");
        check(!report.decisions[3].match
                  && report.decisions[3].disposition == DuplicateDisposition::no_features,
            "nonempty but fully stopped body is no_features, not duplicate");
        check(first.items[0].record->content == raw && second.items[0].record->content == raw,
            "original display bodies survive analysis and decision unchanged");
        check(candidates[0].doc_id == id(0) && candidates.back().doc_id == id(80),
            "batch input order remains unchanged");
        // 复用原创中文词典接线；不声称独立语言学黄金或真实去重质量。
        siftwing::test::Resources resources;
        auto chinese = TextPipeline::create(CppJiebaTokenizer::create(resources.config()).value(),
            StopWords::parse({}, {1, 1, 1, 1}).value())
                           .value();
        const auto cn_a = chinese->analyze("中国 中国 中国 北京", text_limits).value();
        const auto cn_b = chinese->analyze("中国 中国 中国 中文", text_limits).value();
        check(cn_a.tf.terms == FrequencyTable{{"中国", 3}, {"北京", 1}}
                  && cn_b.tf.terms == FrequencyTable{{"中国", 3}, {"中文", 1}},
            "Chinese actual TF upstream features");
        const auto cn =
            decide_duplicates({{id(1), {"中文", 0}, make_simhash(cn_a.tf, hash_limits).value()},
                                  {id(2), {"中文", 1}, make_simhash(cn_b.tf, hash_limits).value()}},
                {},
                {2, 100, 1000, 1})
                .value();
        check(cn.counts.kept == 1 && cn.counts.duplicates == 1 && cn.decisions[1].match
                  && cn.decisions[1].match->distance == 0,
            "dominant Chinese feature duplicate audited");
        // 推荐仍按原选定输入累计；本决策不自动改变推荐统计，不在此构建搜索DF。
        auto recommended =
            accumulate_recommendation({}, cn_a.tf, {100, 100, 100, 1000, 100}).value();
        recommended =
            accumulate_recommendation(recommended, cn_b.tf, {100, 100, 100, 1000, 100}).value();
        check(
            recommended.terms.at("中国") == 6, "recommendation can count both original documents");
    } catch (const std::exception &e) {
        ++failures;
        std::cerr << "unexpected: " << e.what() << '\n';
    }
    std::cout << "duplicate ingestion checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
