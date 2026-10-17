/*
 * PROJECT : SIFTWING
 * FILE    : manifest_ingestion_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 原创文档/偏移/权重与独立推荐累计汇入manifest，核字节绑定及统计分离
 */
#include "siftwing/snapshot/manifest.hpp"
#include "siftwing/document/document_store.hpp"
#include "siftwing/index/weighted_index.hpp"
#include "siftwing/text/text_pipeline.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <iostream>

int main() {
    try {
        using namespace siftwing;
        using namespace snapshot;
        static_assert(document::record_format_version == artifact_layout[0].version);
        static_assert(document::offset_table_version == artifact_layout[1].version);
        static_assert(index::weight_serialization_version == artifact_layout[3].version);
        static_assert(index::weight_policy_version == 1);
        auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({"the\n"}, {100, 10, 10, 100}).value())
                            .value();
        const text::TextPipelineLimits pl{
            {1000, 1000}, {1000, 100, 100, 1000}, {100, 1000, 100, 100, 1000, 100, 1000}};
        const auto tf = pipeline->analyze("THE red RED blue", pl).value().tf;
        const auto did = base::DocumentId::from_integer(0).value();
        // 搜索调用方显式选一篇；推荐明确交付同TF两次，测试不声称实现去重或Writer。
        auto raw = index::InvertedIndex::build(
            {{did, tf}}, {{100, 1000, 100, 100, 1000}, 10, 100, 1000, 10})
                       .value();
        const auto weighted = index::WeightedIndex::build(std::move(raw), {10, 100, 100}).value();
        const auto r1 = text::accumulate_recommendation({}, tf, {10, 100, 100, 1000, 100}).value();
        const auto r2 = text::accumulate_recommendation(r1, tf, {10, 100, 100, 1000, 100}).value();
        const auto rec = index::RecommendationDictionary::build(
            r2, {{100, 1000, 100, 100, 1000}, 10, 100, 100, 100})
                             .value();
        const document::DocumentRecord record{did,
            "原创标题",
            "THE red RED blue",
            {document::SourceKind::txt,
                {"original.txt", 0},
                std::nullopt,
                std::nullopt,
                std::nullopt}};
        const auto library = document::encode_record(record, {1000, 10000}).value();
        const document::OffsetTable table{{{did, 0, static_cast<std::uint64_t>(library.size())}},
            static_cast<std::uint64_t>(library.size())};
        const document::DocumentStoreLimits dl{{1000, 10000}, 10, 100000, 10000};
        const auto offsets = document::encode_offset_table(table, dl).value();
        // 三个payload使用独立手写黄金验证规范，避免用生产Writer编码生成自身期望。
        const std::array<std::string, 6> payloads{{library,
            offsets,
            "SIFTWING-LEXICON 1\nS 2\nT 0 626c7565\nT 1 726564\n",
            weighted.serialize({10000}).value(),
            "SIFTWING-RECOMMENDATION 1\nS 2 6 2\nT 0 626c7565 2\nT 1 726564 4\n",
            "SIFTWING-CHARACTERS 1\nS 6 7\nC 98 0\nC 100 1\nC 101 0 1\nC 108 0\nC 114 1\nC 117 0\n"}};
        const auto s = weighted.raw_index().statistics();
        Manifest m{base::SnapshotVersion::from_integer(42).value(),
            "original-build-1",
            {s.documents,
                static_cast<std::uint64_t>(weighted.raw_index().lexicon().terms().size()),
                s.total_tokens,
                s.total_postings},
            {sha256_bytes("selected-search:a").value(), sha256_bytes("search-config:v1").value()},
            {rec.documents(),
                static_cast<std::uint64_t>(rec.lexicon().terms().size()),
                rec.total_tokens(),
                static_cast<std::uint64_t>(rec.characters().size()),
                7},
            {sha256_bytes("selected-recommendation:a+a").value(),
                sha256_bytes("recommendation-config:v1").value()},
            {}};
        for (std::size_t i = 0; i < 6; ++i) {
            m.files[i] = {artifact_layout[i].version,
                static_cast<std::uint64_t>(payloads[i].size()),
                sha256_bytes(payloads[i]).value()};
        }
        const ManifestLimits ml{10000, 100000, 1000000};
        const auto bytes = serialize_manifest(m, ml).value();
        const auto decoded = parse_manifest(bytes, ml).value();
        unsigned checks = 0, failures = 0;
        const auto check = [&](bool ok) {
            ++checks;
            if (!ok) {
                ++failures;
            }
        };
        check(decoded.search.documents == 1 && decoded.search.total_tokens == 3
              && decoded.search.postings == 2);
        check(decoded.recommendation.documents == 2 && decoded.recommendation.total_tokens == 6
              && decoded.recommendation.character_links == 7);
        check(
            decoded.search_provenance.input_sha256 != decoded.recommendation_provenance.input_sha256
            && decoded.search_provenance.config_sha256
                   != decoded.recommendation_provenance.config_sha256);
        check(rec.lexicon().terms() == std::vector<std::string>{"blue", "red"}
              && rec.frequencies() == std::vector<std::uint64_t>{2, 4});
        check(rec.characters().size() == 6 && rec.characters().at(U'e').size() == 2);
        check(document::decode_offset_table(offsets, dl).value().library_bytes == library.size());
        check(document::validate_record(library, {1000, 10000}).value().doc_id == did);
        for (std::size_t i = 0; i < 6; ++i) {
            check(static_cast<bool>(
                verify_artifact(decoded, static_cast<Artifact>(i), payloads[i], ml)));
        }
        check(
            static_cast<bool>(verify_completion(bytes, completion_marker(bytes, ml).value(), ml)));
        auto changed = payloads[3];
        changed[0] = 'X';
        check(!verify_artifact(decoded, Artifact::search_weights, changed, ml));
        auto old_manifest = m;
        old_manifest.recommendation_provenance.config_sha256 = sha256_bytes("old").value();
        check(!verify_completion(bytes,
            completion_marker(serialize_manifest(old_manifest, ml).value(), ml).value(),
            ml));
        std::cout << "checks=" << checks << " failures=" << failures << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
