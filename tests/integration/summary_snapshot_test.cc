/*
 * PROJECT : SIFTWING
 * FILE    : summary_snapshot_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 原创TXT/RSS经英文/中文建库、快照加载、top-k与原文摘要的接线
 */
#include "../support/cppjieba_resources.hpp"
#include "siftwing/query/summary.hpp"
#include "siftwing/query/top_k.hpp"
#include "siftwing/document/txt_reader.hpp"
#include "siftwing/document/rss_reader.hpp"
#include "siftwing/snapshot/reader.hpp"
#include "siftwing/snapshot/writer.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {
using namespace siftwing;
unsigned checks=0;
void check(bool value,const char *label) { ++checks;if(!value) { throw std::runtime_error(label); } }
base::DocumentId id(std::uint64_t value) { return base::DocumentId::from_integer(value).value(); }
std::vector<query::Summary> run(const std::filesystem::path &parent,bool chinese) {
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    document::TxtDocumentReader txt({{"a.txt",{id(0),"原创TXT"}}});
    document::RssDocumentReader rss({{"feed.xml",{id(maximum)}}},{1000,64,{4096,4096,1000,1000,64}});
    const std::string source="THE 中国 RED red BLUE\r\n";
    const std::string xml="<rss version='2.0'><channel><item><title>原创RSS</title>"
        "<description><![CDATA[<p>北京 blue GREEN</p>]]></description></item></channel></rss>";
    const auto a=document::read_documents({{document::SourceKind::txt,"a.txt",source}},txt,{4096,4096,10,4096},{}).value();
    const auto b=document::read_documents({{document::SourceKind::rss,"feed.xml",xml}},rss,{4096,4096,10,4096},{}).value();
    check(a.items.size()==1 && b.items.size()==1 && a.items[0].record && b.items[0].record,"real adapters");
    auto records=std::vector<document::DocumentRecord>{*a.items[0].record,*b.items[0].record,
        {id(42),"仅标题而缺失正文","",{}}};
    auto stops=text::StopWords::parse({"the\n"},{100,10,10,100}).value();
    std::unique_ptr<text::TextPipeline> pipeline;
    if(chinese) {
        test::Resources resources;
        pipeline=text::TextPipeline::create(text::CppJiebaTokenizer::create(resources.config()).value(),stops).value();
    } else { pipeline=text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),stops).value(); }
    const text::TextPipelineLimits tl{{4096,4096},{4096,100,100,4096},{100,4096,100,100,4096,100,4096}};
    const query::QueryLimits ql{tl,100};
    std::vector<index::IndexDocument> documents;
    for(const auto &record:records) { documents.push_back({record.doc_id,pipeline->analyze(record.content,tl).value().tf}); }
    const index::InvertedIndexLimits il{{1000,10000,100,1000,10000},100,10000,100000,100};
    auto raw=index::InvertedIndex::build(documents,il).value();
    const auto weighted=index::WeightedIndex::build(std::move(raw),{100,1000,1000}).value();
    const auto rec=index::RecommendationDictionary::build({}, {il.lexicon,100,100000,1000,1000}).value();
    const auto digest=snapshot::sha256_bytes("original-summary-fixture").value();
    const snapshot::SnapshotMetadata metadata{base::SnapshotVersion::from_integer(43).value(),
        "summary-fixture-1",{digest,digest},{digest,digest}};
    const snapshot::SnapshotWriteLimits wl{{{4096,10000},100,1000000,100000},{10000,1000000,6000000}};
    check(snapshot::write_snapshot(parent.string(),records,weighted,rec,metadata,wl).value().durability==
        snapshot::PublicationDurability::synced,"published complete snapshot");
    const auto reader=snapshot::SnapshotReader::open(parent.string(),metadata.snapshot,
        {wl.documents,wl.manifest,il,{il.lexicon,100,100000,1000,1000}}).value();
    check(reader.documents()->size()==3,"empty-content record preserved in snapshot");
    const auto before=reader.search()->serialize({100000}).value();
    const query::SummaryLimits limits{4096,100,4096,1000000,100};
    std::vector<query::Summary> out;
    for(const auto &word:std::vector<std::string>{"RED","blue"}) {
        const auto q=query::Query::from_text(word,*pipeline,{query::QueryMode::any,1},ql).value();
        const auto ranked=query::rank_top_k(q,*reader.search(),{{{100,1000,100},10000},100}).value();
        const auto wanted=word=="RED"?id(0):id(maximum);
        check(ranked.size()==1 && ranked[0].document_id==wanted,"top-k to sparse document identity");
        const auto *record=reader.document(ranked[0].document_id);
        check(record!=nullptr,"ranked identity resolves in same snapshot");
        auto summary=query::make_summary(q,record->content,{word=="RED"?7U:4U,0},limits).value();
        // 手写原文的字节偏移：中国/北京各六字节；不从生产分词/摘要取得期望。
        check(summary.text==(word=="RED"?"RED red":"blue"),"original display case preserved");
        check(summary.source_byte_offset==(word=="RED"?11U:7U),"byte offset differs from scalar position");
        check(summary.matched && summary.truncated_before && summary.truncated_after,"summary clipped metadata");
        check(record->content==records[word=="RED"?0U:1U].content,"snapshot original content unchanged");
        out.push_back(std::move(summary));
    }
    const auto unknown=query::Query::from_text("unknown",*pipeline,{query::QueryMode::any,1},ql).value();
    check(query::rank_top_k(unknown,*reader.search(),{{{100,1000,100},10000},100}).value().empty(),"unknown search has no synthetic hit");
    const auto fallback=query::make_summary(unknown,reader.document(id(0))->content,{3,0},limits).value();
    check(fallback.text=="THE" && !fallback.matched && fallback.source_byte_offset==0,"no match raw prefix");
    const auto empty=query::make_summary(unknown,reader.document(id(42))->content,{3,0},limits).value();
    check(empty.text.empty() && !empty.matched && !empty.truncated_before && !empty.truncated_after,
          "missing body does not borrow title");
    check(reader.document(id(99))==nullptr,"caller must handle missing identity before summary");
    if(chinese) {
        const auto q=query::Query::from_text("中国",*pipeline,{query::QueryMode::all,1},ql).value();
        const auto ranked=query::rank_top_k(q,*reader.search(),{{{100,1000,100},10000},100}).value();
        check(ranked.size()==1 && ranked[0].document_id==id(0),"Chinese actual query reaches TXT body");
        const auto summary=query::make_summary(q,reader.document(ranked[0].document_id)->content,{2,0},limits).value();
        check(summary.text=="中国" && summary.source_byte_offset==4 && summary.matched,"Chinese scalar window");
    }
    check(reader.search()->serialize({100000}).value()==before,"summary does not change index");
    check(source=="THE 中国 RED red BLUE\r\n" && xml.find("<p>")!=std::string::npos,"original adapter input preserved");
    return out;
}
} // namespace
int main(int argc,char **argv) {
    try {
        if(argc!=2) { return 2; }
        auto pattern=(std::filesystem::path{argv[1]}/"summary-snapshot-XXXXXX").string();
        const auto *directory=::mkdtemp(pattern.data());
        if(!directory) { throw std::runtime_error("mkdtemp failed"); }
        // 只写新外部派生目录；全部输入原创，不访问原私有语料，输出保留以供诊断。
        for(bool chinese:{false,true}) {
            const auto parent=std::filesystem::path{directory}/(chinese?"chinese":"english");
            check(std::filesystem::create_directory(parent),"new private-free output directory");
            const auto output=run(parent,chinese);
            check(output.size()==2 && output[0].text=="RED red" && output[1].text=="blue",
                  "summary owns text after reader query pipeline destruction");
        }
        std::cout<<"summary snapshot checks="<<checks<<" failures=0\n";return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
