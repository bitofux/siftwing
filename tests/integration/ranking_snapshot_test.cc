/*
 * PROJECT : SIFTWING
 * FILE    : ranking_snapshot_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 原创TXT/RSS英文与中文管线经Writer/Reader验证查询评分接线
 */
#include "../support/cppjieba_resources.hpp"
#include "siftwing/query/ranking.hpp"
#include "siftwing/document/txt_reader.hpp"
#include "siftwing/document/rss_reader.hpp"
#include "siftwing/snapshot/reader.hpp"
#include "siftwing/snapshot/writer.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(label);
    }
}
base::DocumentId id(std::uint64_t number) {
    return base::DocumentId::from_integer(number).value();
}
void near(double actual, double expected, const char *label) {
    check(std::isfinite(actual) && std::abs(actual - expected) < 2e-14, label);
}
query::RankedDocuments run(const std::filesystem::path &parent, unsigned kind) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    document::TxtDocumentReader txt({{"a.txt", {id(0), "原创TXT"}}});
    document::RssDocumentReader rss(
        {{"feed.xml", {id(maximum)}}}, {1000, 64, {4096, 4096, 1000, 1000, 64}});
    const std::string source = "THE 中国 RED red BLUE\r\n";
    const std::string xml =
        "<rss version='2.0'><channel><item><title>原创RSS</title>"
        "<description><![CDATA[<p>北京 blue GREEN</p>]]></description></item></channel></rss>";
    const auto a = document::read_documents(
        {{document::SourceKind::txt, "a.txt", source}}, txt, {4096, 4096, 10, 4096}, {})
                       .value();
    const auto b = document::read_documents(
        {{document::SourceKind::rss, "feed.xml", xml}}, rss, {4096, 4096, 10, 4096}, {})
                       .value();
    check(a.items.size() == 1 && b.items.size() == 1 && a.items[0].record && b.items[0].record,
        "real TXT RSS records");
    const std::vector<document::DocumentRecord> records{*a.items[0].record, *b.items[0].record};
    const auto stops = text::StopWords::parse({"the\n"}, {100, 10, 10, 100}).value();
    std::unique_ptr<text::TextPipeline> pipeline;
    if (kind == 0) {
        pipeline =
            text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(), stops).value();
    } else {
        test::Resources resources;
        pipeline = text::TextPipeline::create(
            text::CppJiebaTokenizer::create(resources.config()).value(), stops)
                       .value();
    }
    const text::TextPipelineLimits tl{
        {4096, 4096}, {4096, 100, 100, 4096}, {100, 4096, 100, 100, 4096, 100, 4096}};
    const query::QueryLimits ql{tl, 100};
    std::vector<index::IndexDocument> documents;
    for (const auto &record : records) {
        documents.push_back({record.doc_id, pipeline->analyze(record.content, tl).value().tf});
    }
    const index::InvertedIndexLimits il{{1000, 10000, 100, 1000, 10000}, 100, 10000, 100000, 100};
    auto raw = index::InvertedIndex::build(documents, il).value();
    const auto weighted = index::WeightedIndex::build(std::move(raw), {100, 1000, 1000}).value();
    const auto rec =
        index::RecommendationDictionary::build({}, {il.lexicon, 100, 100000, 1000, 1000}).value();
    const auto digest = snapshot::sha256_bytes("original-ranking-fixture").value();
    const snapshot::SnapshotMetadata metadata{base::SnapshotVersion::from_integer(42).value(),
        "ranking-fixture-1",
        {digest, digest},
        {digest, digest}};
    const snapshot::SnapshotWriteLimits wl{
        {{4096, 10000}, 100, 1000000, 100000}, {10000, 1000000, 6000000}};
    const auto published =
        snapshot::write_snapshot(parent.string(), records, weighted, rec, metadata, wl).value();
    check(published.durability == snapshot::PublicationDurability::synced, "snapshot published");
    const auto reader = snapshot::SnapshotReader::open(parent.string(),
        metadata.snapshot,
        {wl.documents, wl.manifest, il, {il.lexicon, 100, 100000, 1000, 1000}})
                            .value();
    check(reader.search() && reader.documents() && reader.documents()->size() == 2,
        "reader owns loaded search");
    const auto before = reader.search()->serialize({100000}).value();
    const auto rank = [&](std::string_view text, query::QueryMode mode = query::QueryMode::any) {
        const auto q = query::Query::from_text(text, *pipeline, {mode, 1}, ql).value();
        return query::rank_candidates(q, *reader.search(), {{100, 1000, 100}, 10000}).value();
    };
    // 手写TF：英文doc0={red:2,blue:1}，docMAX={green:1,blue:1}；中文各多一个独有词。
    // N=2，blue的DF=2/IDF=1，独有词DF=1/IDF=h，不读取生产IDF或norm构造期望。
    const double h = 1 + std::log(1.5);
    const double n0 = std::sqrt(1 + (kind == 0 ? 4.0 : 5.0) * h * h);
    const double nmax = std::sqrt(1 + (kind == 0 ? 1.0 : 2.0) * h * h);
    for (auto mode : {query::QueryMode::all, query::QueryMode::any}) {
        const auto blue = rank("BLUE blue", mode);
        check(
            blue.size() == 2 && blue[0].document_id == id(maximum) && blue[1].document_id == id(0),
            "full scores reorder sparse endpoints even top_k one");
        near(blue[0].score, 1 / nmax, "loaded blue MAX score");
        near(blue[1].score, 1 / n0, "loaded blue zero score");
        const auto red = rank("red RED", mode);
        check(red.size() == 1 && red[0].document_id == id(0), "loaded repeated red one match");
        near(red[0].score, 2 * h / n0, "loaded repeated red score");
        check(rank("unknown", mode).empty(), "loaded all unknown empty");
    }
    const auto both = rank("red green");
    check(both.size() == 2 && both.front().document_id == id(0),
        "OR relevance crosses original adapters");
    near(both[0].score, std::sqrt(2.0) * h / n0, "loaded two-dimension query doc0");
    near(both[1].score, h / (std::sqrt(2.0) * nmax), "loaded two-dimension query MAX");
    check(rank("red green", query::QueryMode::all).empty(), "AND disjoint empty");
    const auto partial = rank("blue unknown unknown");
    near(partial[0].score, 1 / nmax, "loaded OR unknown not in norm");
    check(rank("blue unknown", query::QueryMode::all).empty(), "loaded AND unknown empty");
    if (kind == 1) {
        const auto chinese = rank("中国 北京");
        check(chinese.size() == 2 && chinese.front().document_id == id(maximum),
            "Chinese query terms across TXT RSS");
        near(chinese[0].score, h / (std::sqrt(2.0) * nmax), "Chinese loaded MAX cosine");
        near(chinese[1].score, h / (std::sqrt(2.0) * n0), "Chinese loaded zero cosine");
        const auto mixed = rank("中国 blue", query::QueryMode::all);
        check(mixed.size() == 1 && mixed[0].document_id == id(0),
            "mixed-language AND uses loaded vocabulary");
        near(mixed[0].score, std::sqrt(1 + h * h) / n0, "mixed-language loaded cosine");
    }
    check(reader.search()->serialize({100000}).value() == before,
        "ranking preserves canonical loaded index");
    check(source == "THE 中国 RED red BLUE\r\n" && xml.find("<p>") != std::string::npos,
        "original fixture bytes preserved");
    return rank("blue");
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            return 2;
        }
        // 所有输入原创；每次创建新的外部输出目录，保留旧产物，不接触私有业务语料。
        auto pattern = (std::filesystem::path{argv[1]} / "ranking-snapshot-XXXXXX").string();
        const auto *directory = ::mkdtemp(pattern.data());
        if (!directory) {
            throw std::runtime_error("mkdtemp failed");
        }
        for (unsigned kind = 0; kind < 2; ++kind) {
            const auto parent = std::filesystem::path{directory} / std::to_string(kind);
            if (!std::filesystem::create_directory(parent)) {
                throw std::runtime_error("fixture directory exists");
            }
            const auto owned = run(parent, kind);
            check(owned.size() == 2
                      && owned[0].document_id == id(std::numeric_limits<std::uint64_t>::max())
                      && owned[0].score > owned[1].score,
                "scores survive query pipeline index reader destruction");
        }
        std::cout << "ranking snapshot checks=" << checks << " failures=0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
