/*
 * PROJECT : SIFTWING
 * FILE    : recommendation_snapshot_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 原创TXT/RSS中英文独立推荐累计经Writer/Reader接入候选词查询
 */
#include "../support/cppjieba_resources.hpp"
#include "siftwing/query/recommendation.hpp"
#include "siftwing/document/txt_reader.hpp"
#include "siftwing/document/rss_reader.hpp"
#include "siftwing/snapshot/reader.hpp"
#include "siftwing/snapshot/writer.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include "siftwing/text/text_pipeline.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        throw std::runtime_error(label);
    }
}
base::DocumentId id(std::uint64_t n) {
    return base::DocumentId::from_integer(n).value();
}
query::RecommendedTerms run(const std::filesystem::path &parent, bool chinese) {
    document::TxtDocumentReader txt({{"a.txt", {id(0), "原创TXT"}}});
    document::RssDocumentReader rss({{"feed.xml", {id(std::numeric_limits<std::uint64_t>::max())}}},
        {1000, 64, {4096, 4096, 1000, 1000, 64}});
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
        "TXT RSS records");
    const std::vector<document::DocumentRecord> records{*a.items[0].record, *b.items[0].record};
    const auto stops = text::StopWords::parse({"the\n"}, {100, 10, 10, 100}).value();
    std::unique_ptr<text::TextPipeline> pipeline;
    if (chinese) {
        test::Resources resources;
        pipeline = text::TextPipeline::create(
            text::CppJiebaTokenizer::create(resources.config()).value(), stops)
                       .value();
    } else {
        pipeline =
            text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(), stops).value();
    }
    const text::TextPipelineLimits tl{
        {4096, 4096}, {4096, 100, 100, 4096}, {100, 4096, 100, 100, 4096, 100, 4096}};
    const auto first = pipeline->analyze(records[0].content, tl).value();
    const auto second = pipeline->analyze(records[1].content, tl).value();
    const auto empty = pipeline->analyze("THE", tl).value();
    check(first.tf.terms.at("red") == 2 && first.tf.terms.at("blue") == 1
              && second.tf.terms.at("blue") == 1 && second.tf.terms.at("green") == 1,
        "original hand computed TF");
    const index::InvertedIndexLimits il{{100, 10000, 100, 100, 10000}, 100, 10000, 100000, 100};
    auto raw = index::InvertedIndex::build(
        {{id(0), first.tf}, {id(std::numeric_limits<std::uint64_t>::max()), second.tf}}, il)
                   .value();
    const auto weighted = index::WeightedIndex::build(std::move(raw), {100, 1000, 1000}).value();
    // 推荐显式交付TXT、RSS、重复TXT、全停用篇；不自动采用搜索N或搜索去重后频次。
    const text::RecommendationLimits accumulation{100, 1000, 100, 10000, 1000};
    const auto r1 = text::accumulate_recommendation({}, first.tf, accumulation).value();
    const auto r2 = text::accumulate_recommendation(r1, second.tf, accumulation).value();
    const auto r3 = text::accumulate_recommendation(r2, first.tf, accumulation).value();
    const auto r4 = text::accumulate_recommendation(r3, empty.tf, accumulation).value();
    const index::RecommendationDictionaryLimits dl{il.lexicon, 100, 10000, 1000, 1000};
    const auto rec = index::RecommendationDictionary::build(r4, dl).value();
    check(rec.documents() == 4 && rec.total_tokens() == (chinese ? 11U : 8U),
        "recommendation deliveries independent");
    const auto digest = snapshot::sha256_bytes("original-recommendation-fixture").value();
    const snapshot::SnapshotMetadata metadata{base::SnapshotVersion::from_integer(42).value(),
        "recommendation-fixture-1",
        {digest, digest},
        {digest, digest}};
    const snapshot::SnapshotWriteLimits wl{
        {{4096, 10000}, 100, 1000000, 100000}, {10000, 1000000, 6000000}};
    const auto written =
        snapshot::write_snapshot(parent.string(), records, weighted, rec, metadata, wl).value();
    check(written.durability == snapshot::PublicationDurability::synced, "snapshot synced");
    const auto reader = snapshot::SnapshotReader::open(
        parent.string(), metadata.snapshot, {wl.documents, wl.manifest, il, dl})
                            .value();
    check(reader.recommendation() && reader.search() && reader.documents()->size() == 2,
        "reader loaded both branches");
    const auto &loaded = *reader.recommendation();
    check(loaded.documents() == 4 && loaded.total_tokens() == rec.total_tokens(),
        "loaded original recommendation stats");
    check(loaded.lexicon().terms() == rec.lexicon().terms()
              && loaded.frequencies() == rec.frequencies()
              && loaded.characters() == rec.characters(),
        "loaded recommendation relation identity");
    const auto before = reader.search()->serialize({100000}).value();
    const auto chars = loaded.characters();
    const auto counts = loaded.frequencies();
    const query::RecommendationLimits limits{4096, 1000, 100, 1000, 1000, 10000, 100000, 10000};
    const auto result = query::recommend_terms("ree", loaded, 10, limits).value();
    check(result.size() == 3, "loaded shared characters union includes blue sharing e");
    check(result[0].term == "red" && result[0].distance == 1 && result[0].frequency == 4,
        "loaded best hand distance and rec freq");
    check(result[1].term == "green" && result[1].distance == 2 && result[1].frequency == 1,
        "loaded next hand distance");
    check(result[2].term == "blue" && result[2].distance == 3 && result[2].frequency == 3,
        "loaded blue also shares e");
    for (const auto &entry : result) {
        check(loaded.lexicon().term(entry.term_id) == entry.term
                  && loaded.frequency(entry.term_id) == entry.frequency,
            "result ID belongs to recommendation");
    }
    check(query::recommend_terms("Ω", loaded, 1, limits).value().empty(),
        "loaded no shared character empty");
    const auto canonical = text::normalize_utf8("BLUE", {100, 100}).value();
    const auto blue = query::recommend_terms(canonical, loaded, 1, limits).value();
    check(blue.size() == 1 && blue.front().term == "blue" && blue.front().distance == 0
              && blue.front().frequency == 3,
        "caller explicitly prepares normalized key");
    const auto bad = query::recommend_terms("blue green", loaded, 1, limits);
    check(!bad && bad.error().context == "recommend.query", "single word contract across snapshot");
    if (chinese) {
        const auto cn = query::recommend_terms("中", loaded, 1, limits).value();
        check(cn.size() == 1 && cn.front().term == "中国" && cn.front().distance == 1
                  && cn.front().frequency == 2,
            "loaded Chinese scalar distance");
        const auto mixed = query::recommend_terms("中red", loaded, 10, limits).value();
        check(mixed.size() == 4 && mixed.front().term == "red" && mixed.front().distance == 1,
            "loaded mixed Unicode candidate union");
    }
    check(reader.search()->serialize({100000}).value() == before && loaded.characters() == chars
              && loaded.frequencies() == counts,
        "recommendation preserves both loaded branches");
    check(source == "THE 中国 RED red BLUE\r\n" && xml.find("<p>") != std::string::npos,
        "original input bytes unchanged");
    return result;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            return 2;
        }
        auto pattern = (std::filesystem::path{argv[1]} / "recommendation-snapshot-XXXXXX").string();
        const auto *directory = ::mkdtemp(pattern.data());
        if (!directory) {
            throw std::runtime_error("mkdtemp failed");
        }
        for (bool chinese : {false, true}) {
            const auto parent =
                std::filesystem::path{directory} / (chinese ? "chinese" : "english");
            if (!std::filesystem::create_directory(parent)) {
                throw std::runtime_error("fixture directory exists");
            }
            const auto owned = run(parent, chinese);
            check(owned.front().term == "red" && owned.front().frequency == 4,
                "suggestions survive Reader/pipeline/dictionary destruction");
        }
        std::cout << "recommendation snapshot checks=" << checks << " failures=0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
