/*
 * PROJECT : SIFTWING
 * FILE    : writer_ingestion_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 原创文本管线分别形成搜索/推荐产物，真实发布后复用已有文档读者核正文及manifest字节
 */
#include "siftwing/snapshot/writer.hpp"
#include "siftwing/text/text_pipeline.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
std::string load(const std::filesystem::path &path) {
    std::ifstream input(path,std::ios::binary);
    if (!input) { throw std::runtime_error("cannot open integration artifact"); }
    return {std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
}
} // namespace
int main(int argc,char **argv) {
    try {
        if (argc != 2) { return 2; }
        using namespace siftwing;
        namespace fs = std::filesystem;
        std::string pattern = (fs::path{argv[1]}/"writer-ingestion-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) { return 2; }
        const fs::path parent{pattern};
        auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({"the\n"},{100,10,10,100}).value()).value();
        const text::TextPipelineLimits pl{{1000,1000},{1000,100,100,1000},{100,1000,100,100,1000,100,1000}};
        const auto tf = pipeline->analyze("THE red RED blue",pl).value().tf;
        const auto id = base::DocumentId::from_integer(7).value();
        auto raw = index::InvertedIndex::build({{id,tf}},{{100,1000,100,100,1000},10,100,1000,10}).value();
        const auto weighted = index::WeightedIndex::build(std::move(raw),{10,100,100}).value();
        const auto first = text::accumulate_recommendation({},tf,{10,100,100,1000,100}).value();
        const auto second = text::accumulate_recommendation(first,tf,{10,100,100,1000,100}).value();
        const auto rec = index::RecommendationDictionary::build(second,{{100,1000,100,100,1000},10,100,100,100}).value();
        const std::vector<document::DocumentRecord> records{{id,"原创标题","THE red RED blue",
            {document::SourceKind::txt,{"original.txt",0},std::nullopt,std::string{},std::nullopt}}};
        const snapshot::SnapshotMetadata metadata{base::SnapshotVersion::from_integer(42).value(),"integration-original-1",
            {snapshot::sha256_bytes("selected-search:a").value(),snapshot::sha256_bytes("search-config:v1").value()},
            {snapshot::sha256_bytes("selected-recommendation:a+a").value(),snapshot::sha256_bytes("recommendation-config:v1").value()}};
        const snapshot::SnapshotWriteLimits limits{{{1000,10000},10,100000,10000},{10000,100000,1000000}};
        const auto result = snapshot::write_snapshot(pattern,records,weighted,rec,metadata,limits);
        unsigned checks = 0, failures = 0;
        const auto check = [&](bool ok) { ++checks; if (!ok) { ++failures; } };
        check(result && result.value().durability == snapshot::PublicationDurability::synced);
        const auto folder = parent/"42"; const auto bytes = load(folder/"manifest.txt");
        const auto manifest = snapshot::parse_manifest(bytes,limits.manifest).value();
        check(manifest.search.documents == 1 && manifest.search.total_tokens == 3 && manifest.search.postings == 2);
        check(manifest.recommendation.documents == 2 && manifest.recommendation.total_tokens == 6 && manifest.recommendation.character_links == 7);
        check(manifest.search_provenance.input_sha256 != manifest.recommendation_provenance.input_sha256);
        check(manifest.search_provenance.config_sha256 != manifest.recommendation_provenance.config_sha256);
        check(load(folder/"search-lexicon.txt") == "SIFTWING-LEXICON 1\nS 2\nT 0 626c7565\nT 1 726564\n");
        check(load(folder/"recommendation-dictionary.txt") == "SIFTWING-RECOMMENDATION 1\nS 2 6 2\nT 0 626c7565 2\nT 1 726564 4\n");
        check(load(folder/"recommendation-characters.txt") == "SIFTWING-CHARACTERS 1\nS 6 7\nC 98 0\nC 100 1\nC 101 0 1\nC 108 0\nC 114 1\nC 117 0\n");
        for (std::size_t i = 0; i < snapshot::artifact_layout.size(); ++i) {
            check(static_cast<bool>(snapshot::verify_artifact(manifest,static_cast<snapshot::Artifact>(i),
                load(folder/std::string{snapshot::artifact_layout[i].name}),limits.manifest)));
        }
        check(static_cast<bool>(snapshot::verify_completion(bytes,load(folder/"COMPLETE"),limits.manifest)));
        const auto table = document::decode_offset_table(load(folder/"offsets.bin"),limits.documents).value();
        {
            const auto reader = document::DocumentStoreReader::open((folder/"documents.bin").string(),table,limits.documents).value();
            const auto record = reader.read(id).value();
            check(record.doc_id == id && record.title == records[0].title && record.content == records[0].content);
            check(record.source.source_id.input_path == "original.txt" && record.source.author && record.source.author->empty() && !record.source.url);
        }
        auto independent = result.value(); independent.manifest.build_version = "copy-change";
        check(result.value().manifest.build_version == "integration-original-1");
        check(weighted.raw_index().statistics().total_tokens == 3 && rec.total_tokens() == 6 && records[0].content == "THE red RED blue");
        fs::remove_all(parent); // 仅本测试新建输出，未使用原始私有资料或新的SnapshotReader。
        std::cout << "checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
