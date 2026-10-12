/*
 * PROJECT : SIFTWING
 * FILE    : candidates_snapshot_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 原创TXT/RSS与英文/中文共享管线经快照加载后匹配文档集合
 */
#include "../support/cppjieba_resources.h"
#include "siftwing/query/candidates.h"
#include "siftwing/document/txt_reader.h"
#include "siftwing/document/rss_reader.h"
#include "siftwing/snapshot/reader.h"
#include "siftwing/snapshot/writer.h"
#include "siftwing/text/english_tokenizer.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing;
unsigned checks=0;
void check(bool condition,const char *label) {
    ++checks;if(!condition) { throw std::runtime_error(label); }
}
base::DocumentId id(std::uint64_t number) { return base::DocumentId::from_integer(number).value(); }
/** @brief 分别验证英文与原创中文资源；期望集合由输入手写，不从索引重新推导。 */
query::CandidateDocuments run(const std::filesystem::path &parent,unsigned kind) {
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    document::TxtDocumentReader txt({{"a.txt",{id(0),"原创TXT"}}});
    document::RssDocumentReader rss({{"feed.xml",{id(maximum)}}},{1000,64,{4096,4096,1000,1000,64}});
    const std::string source="THE 中国 RED red BLUE\r\n";
    const std::string xml="<rss version='2.0'><channel><item><title>原创RSS</title>"
        "<description><![CDATA[<p>北京 blue GREEN</p>]]></description></item></channel></rss>";
    const auto a=document::read_documents({{document::SourceKind::txt,"a.txt",source}},txt,{4096,4096,10,4096},{}).value();
    const auto b=document::read_documents({{document::SourceKind::rss,"feed.xml",xml}},rss,{4096,4096,10,4096},{}).value();
    check(a.items.size()==1 && b.items.size()==1 && a.items[0].record && b.items[0].record,"real adapter records");
    const std::vector<document::DocumentRecord> records{*a.items[0].record,*b.items[0].record};
    const auto stops=text::StopWords::parse({"the\n"},{100,10,10,100}).value();
    std::unique_ptr<text::TextPipeline> pipeline;
    if(kind==0) {
        pipeline=text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),stops).value();
    } else {
        test::Resources resources;
        pipeline=text::TextPipeline::create(text::CppJiebaTokenizer::create(resources.config()).value(),stops).value();
    }
    const text::TextPipelineLimits tl{{4096,4096},{4096,100,100,4096},{100,4096,100,100,4096,100,4096}};
    const query::QueryLimits ql{tl,100};
    std::vector<index::IndexDocument> documents;
    for(const auto &record:records) { documents.push_back({record.doc_id,pipeline->analyze(record.content,tl).value().tf}); }
    const index::InvertedIndexLimits il{{1000,10000,100,1000,10000},100,10000,100000,100};
    auto raw=index::InvertedIndex::build(documents,il).value();
    const auto weighted=index::WeightedIndex::build(std::move(raw),{100,1000,1000}).value();
    const auto rec=index::RecommendationDictionary::build({}, {il.lexicon,100,100000,1000,1000}).value();
    const auto digest=snapshot::sha256_bytes("original-candidate-fixture").value();
    const snapshot::SnapshotMetadata metadata{base::SnapshotVersion::from_integer(42).value(),
        "candidate-fixture-1",{digest,digest},{digest,digest}};
    const snapshot::SnapshotWriteLimits wl{{{4096,10000},100,1000000,100000},{10000,1000000,6000000}};
    const auto published=snapshot::write_snapshot(parent.string(),records,weighted,rec,metadata,wl).value();
    check(published.durability==snapshot::PublicationDurability::synced,"snapshot published");
    const auto reader=snapshot::SnapshotReader::open(parent.string(),metadata.snapshot,
        {wl.documents,wl.manifest,il,{il.lexicon,100,100000,1000,1000}}).value();
    check(reader.search() && reader.documents() && reader.documents()->size()==2,"full reader owns search and records");
    const auto before=reader.search()->serialize({100000}).value();
    const auto &index=reader.search()->raw_index();
    const auto match=[&](std::string_view text,query::QueryMode mode) {
        const auto q=query::Query::from_text(text,*pipeline,{mode,1},ql).value();
        return query::collect_candidates(q,index,{100,1000,100}).value();
    };
    for(auto mode:{query::QueryMode::all,query::QueryMode::any}) {
        check(match("BLUE blue",mode)==query::CandidateDocuments{id(0),id(maximum)},"same word overlaps two sparse endpoint documents");
        check(match("red RED",mode)==query::CandidateDocuments{id(0)},"repeat query preserves one constraint");
        check(match("unknown",mode).empty(),"all unknown successful empty");
    }
    check(match("red green",query::QueryMode::any)==query::CandidateDocuments{id(0),id(maximum)},"OR across TXT and RSS");
    check(match("red green",query::QueryMode::all).empty(),"AND disjoint documents");
    check(match("blue unknown",query::QueryMode::any).size()==2,"OR ignores unknown");
    check(match("blue unknown",query::QueryMode::all).empty(),"AND unknown blocks");
    if(kind==1) {
        check(match("中国 blue",query::QueryMode::all)==query::CandidateDocuments{id(0)},"Chinese AND shares actual dictionary keys");
        check(match("中国 北京",query::QueryMode::any)==query::CandidateDocuments{id(0),id(maximum)},"Chinese OR spans two records");
    }
    check(reader.search()->serialize({100000}).value()==before,"matching preserves loaded canonical index bytes");
    check(source=="THE 中国 RED red BLUE\r\n" && xml.find("<p>")!=std::string::npos,"original fixture bytes preserved");
    return match("blue",query::QueryMode::any);
}
} // namespace
int main(int argc,char **argv) {
    try {
        if(argc!=2) { return 2; }
        // 每次运行创建全新外部输出目录，旧产物保留；不删除或覆盖已有快照。
        auto pattern=(std::filesystem::path{argv[1]}/"candidate-snapshot-XXXXXX").string();
        const auto *directory=::mkdtemp(pattern.data());
        if(!directory) { throw std::runtime_error("mkdtemp failed"); }
        for(unsigned kind=0;kind<2;++kind) {
            const auto parent=std::filesystem::path{directory}/std::to_string(kind);
            if(!std::filesystem::create_directory(parent)) { throw std::runtime_error("fixture directory exists"); }
            const auto owned=run(parent,kind);
            check(owned.size()==2 && owned.front()==id(0) && owned.back()==id(std::numeric_limits<std::uint64_t>::max()),
                "output survives pipeline query reader and index destruction");
        }
        std::cout<<"candidate snapshot checks="<<checks<<" failures=0\n";return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
