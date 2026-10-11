/*
 * PROJECT : SIFTWING
 * FILE    : roundtrip_pipeline_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 原创TXT/RSS经文本、真实去重和独立推荐累计形成两份快照，再完整加载
 */
#include "siftwing/document/txt_reader.h"
#include "siftwing/document/rss_reader.h"
#include "siftwing/document/duplicate_decision.h"
#include "siftwing/snapshot/reader.h"
#include "siftwing/snapshot/writer.h"
#include "siftwing/text/text_pipeline.h"
#include "siftwing/text/english_tokenizer.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) { throw std::runtime_error(label); }
}
base::DocumentId id(std::uint64_t value) { return base::DocumentId::from_integer(value).value(); }

/** @brief 每次重建Reader和管线；反转批量输入和去重候选，不依赖上一次的内存产物。 */
void build(const std::filesystem::path &parent, bool reverse) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::string first = "\xef\xbb\xbfTHE red RED blue\r\n";
    const std::string second = "the red red blue\n";
    const std::string filtered = "THE the";
    const std::string xml = "<rss version='2.0'><channel><item><title> RSS </title>"
        "<description><![CDATA[<p>Green blue</p>]]></description><link/>"
        "<author> Writer </author><pubDate> raw date </pubDate></item></channel></rss>";
    document::TxtDocumentReader txt({{"a.txt",{id(0),"Alpha"}},
        {"b.txt",{id(9),"Duplicate"}},{"z.txt",{id(maximum),"Filtered"}}});
    const document::ReadLimits incoming{10000,10000,100,100000};
    std::vector<document::ReaderInput> inputs{{document::SourceKind::txt,"a.txt",first},
        {document::SourceKind::txt,"b.txt",second},{document::SourceKind::txt,"z.txt",filtered}};
    if (reverse) { std::reverse(inputs.begin(),inputs.end()); }
    const auto text_report = document::read_documents(inputs,txt,incoming,{}).value();
    document::RssDocumentReader rss({{"feed.xml",{id(77)}}},{1000,64,{10000,10000,1000,1000,64}});
    const auto rss_report = document::read_documents(
        {{document::SourceKind::rss,"feed.xml",xml}},rss,incoming,{}).value();
    check(!text_report.stop && !rss_report.stop,"all input items processed");
    check(text_report.items.size()==3 && rss_report.items.size()==1,"four delivered records");
    std::vector<document::DocumentRecord> originals;
    for (const auto &report : {&text_report,&rss_report}) {
        for (const auto &item : report->items) {
            check(item.record.has_value(),"all original records accepted");
            originals.push_back(*item.record);
        }
    }
    check(originals[0].content=="THE red RED blue\n","TXT BOM and CRLF extraction");
    check(originals.back().title=="RSS" && originals.back().content=="Green blue",
        "RSS title trimming and HTML extraction");
    auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
        text::StopWords::parse({"the\n"},{100,10,10,100}).value()).value();
    const text::TextPipelineLimits pl{{10000,10000},{10000,100,100,10000},
        {100,10000,100,100,10000,100,10000}};
    std::vector<document::DuplicateCandidate> candidates;
    std::vector<index::IndexDocument> analyzed;
    text::RecommendationFrequencies accumulated;
    for (const auto &record : originals) {
        const auto tf = pipeline->analyze(record.content,pl).value().tf;
        const auto fingerprint = document::make_simhash(tf,{100,100,10000,10000}).value();
        candidates.push_back({record.doc_id,record.source.source_id,fingerprint});
        analyzed.push_back({record.doc_id,tf});
        accumulated = text::accumulate_recommendation(accumulated,tf,{100,100,100,10000,10000}).value();
    }
    if (reverse) { std::reverse(candidates.begin(),candidates.end()); }
    const auto decisions = document::decide_duplicates(candidates,{0},{100,100,10000,10000}).value();
    check(decisions.counts.kept==2 && decisions.counts.duplicates==1 &&
        decisions.counts.no_features==1,"exact duplicate and filtered document have separate outcomes");
    check(accumulated.documents==4 && accumulated.total_tokens==8 &&
        accumulated.terms==text::FrequencyTable{{"blue",3},{"green",1},{"red",4}},
        "recommendation includes duplicate delivery and empty TF");
    std::vector<document::DocumentRecord> retained;
    std::vector<index::IndexDocument> search_input;
    for (const auto &decision : decisions.decisions) {
        if (decision.disposition != document::DuplicateDisposition::kept) { continue; }
        for (std::size_t i=0;i<originals.size();++i) {
            if (originals[i].doc_id==decision.doc_id) {
                retained.push_back(originals[i]); search_input.push_back(analyzed[i]);
            }
        }
    }
    if (reverse) { std::reverse(retained.begin(),retained.end()); std::reverse(search_input.begin(),search_input.end()); }
    const index::LexiconLimits ll{1000,10000,100,1000,10000};
    auto raw = index::InvertedIndex::build(search_input,{ll,100,10000,100000,100}).value();
    const auto search = index::WeightedIndex::build(std::move(raw),{100,1000,1000}).value();
    const auto recommendation = index::RecommendationDictionary::build(accumulated,
        {ll,100,100000,1000,1000}).value();
    const snapshot::SnapshotMetadata metadata{base::SnapshotVersion::from_integer(42).value(),
        "roundtrip-pipeline-1",{snapshot::sha256_bytes("search:a.txt+feed.xml").value(),
        snapshot::sha256_bytes("english:stop-the:simhash-distance-0").value()},
        {snapshot::sha256_bytes("recommendation:a.txt+b.txt+feed.xml+z.txt").value(),
        snapshot::sha256_bytes("english:stop-the:all-deliveries").value()}};
    const snapshot::SnapshotWriteLimits wl{{{10000,100000},100,1000000,100000},
        {10000,1000000,6000000}};
    const auto published = snapshot::write_snapshot(parent.string(),retained,search,recommendation,metadata,wl).value();
    check(published.durability==snapshot::PublicationDurability::synced,"publication synchronized");
    const snapshot::SnapshotReadLimits rl{wl.documents,wl.manifest,{ll,100,10000,100000,100},
        {ll,100,100000,1000,1000}};
    const auto reader = snapshot::SnapshotReader::open(parent.string(),metadata.snapshot,rl).value();
    check(reader.manifest()->search.documents==2 && reader.manifest()->search.total_tokens==5 &&
        reader.manifest()->search.postings==4,"loaded search selection and integer statistics");
    check(reader.recommendation()->documents()==4 && reader.recommendation()->total_tokens()==8,
        "loaded recommendation selection remains independent");
    check(reader.search()->serialize({100000}).value()==search.serialize({100000}).value(),
        "loaded search matches pre-save canonical bytes");
    check(reader.recommendation()->lexicon().terms()==recommendation.lexicon().terms() &&
        reader.recommendation()->frequencies()==recommendation.frequencies() &&
        reader.recommendation()->characters()==recommendation.characters(),"all loaded recommendation relationships");
    for (const auto &record : retained) {
        const auto loaded = reader.document(record.doc_id);
        check(loaded && document::encode_record(*loaded,wl.documents.record).value()==
            document::encode_record(record,wl.documents.record).value(),"all record fields and optional presence survive");
    }
    check(!reader.document(id(9)) && !reader.document(id(maximum)),"discarded IDs absent from search records");
    check(first=="\xef\xbb\xbfTHE red RED blue\r\n" && second=="the red red blue\n" &&
        filtered=="THE the","original input remains unchanged");
}
} // namespace

int main(int argc,char **argv) {
    try {
        if (argc!=2) { return 2; }
        // 驱动提供专属全新输出根；本程序不覆盖、不删除已有目录，失败由驱动保留诊断。
        for (unsigned iteration=0;iteration<2;++iteration) {
            const auto parent = std::filesystem::path{argv[1]}/("pipeline-"+std::to_string(iteration));
            if (!std::filesystem::create_directory(parent)) { throw std::runtime_error("test output already exists"); }
            build(parent,iteration!=0);
        }
        std::cout << "checks=" << checks << " failures=0\n"; return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
