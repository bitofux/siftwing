/*
 * PROJECT : SIFTWING
 * FILE    : reader_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 将真实Reader的拥有部件导出给独立Python参考，不借用Writer编码
 */
#include "siftwing/snapshot/reader.hpp"
#include <array>
#include <charconv>
#include <iostream>
#include <limits>
using namespace siftwing;
namespace {
std::string hex(std::string_view value) {
    constexpr char digits[]="0123456789abcdef"; std::string out;
    for (unsigned char byte:value) { out.push_back(digits[byte>>4U]); out.push_back(digits[byte&15U]); } return out;
}
bool integer(const char *text,std::uint64_t &value) {
    const std::string_view bytes{text}; const auto r=std::from_chars(bytes.data(),bytes.data()+bytes.size(),value);
    return !bytes.empty() && r.ec==std::errc{} && r.ptr==bytes.data()+bytes.size();
}
}
int main(int argc,char **argv) {
    if (argc!=3 && argc!=5) { return 3; } std::uint64_t version=0;
    if (!integer(argv[2],version)) { return 3; }
    const auto huge=std::numeric_limits<std::uint64_t>::max(); constexpr std::uint64_t bytes=1ULL<<26U;
    snapshot::SnapshotReadLimits limits{{{bytes,bytes},1000000,bytes,bytes},{bytes,bytes,bytes},
        {{1000000,bytes,bytes,1000000,bytes},1000000,huge,huge,1000000},
        {{1000000,bytes,bytes,1000000,bytes},huge,huge,1000000,1000000}};
    auto &d=limits.documents; auto &m=limits.manifest; auto &s=limits.search; auto &r=limits.recommendation;
    // 顺序固定为公开聚合中的26预算，仅测试探针参数，不构成生产配置格式。
    const std::array<std::uint64_t *,26> fields{{&d.record.max_field_bytes,&d.record.max_record_bytes,&d.max_documents,
        &d.max_library_bytes,&d.max_table_bytes,&m.max_manifest_bytes,&m.max_file_bytes,&m.max_total_file_bytes,
        &s.lexicon.max_input_terms,&s.lexicon.max_input_bytes,&s.lexicon.max_term_bytes,&s.lexicon.max_distinct_terms,
        &s.lexicon.max_vocabulary_bytes,&s.max_documents,&s.max_document_tokens,&s.max_total_tokens,&s.max_postings_per_term,
        &r.lexicon.max_input_terms,&r.lexicon.max_input_bytes,&r.lexicon.max_term_bytes,&r.lexicon.max_distinct_terms,
        &r.lexicon.max_vocabulary_bytes,&r.max_documents,&r.max_total_tokens,&r.max_characters,&r.max_character_links}};
    if (argc==5) { std::uint64_t slot=0,value=0; if (!integer(argv[3],slot) || !integer(argv[4],value) || slot>=fields.size()) { return 3; }
        *fields[static_cast<std::size_t>(slot)]=value; }
    auto loaded=snapshot::SnapshotReader::open(argv[1],base::SnapshotVersion::from_integer(version).value(),limits);
    if (!loaded) { std::cout<<"REJECT "<<loaded.error().context<<'\n'; return 1; }
    const auto &reader=loaded.value(); auto manifest=snapshot::serialize_manifest(*reader.manifest(),limits.manifest);
    auto weights=reader.search()->serialize({limits.manifest.max_file_bytes}); if (!manifest || !weights) { return 2; }
    std::cout<<"OK\nM "<<hex(manifest.value())<<"\nW "<<hex(weights.value())<<'\n';
    for (const auto &doc:*reader.documents()) {
        auto encoded=document::encode_record(doc,limits.documents.record); if (!encoded || reader.document(doc.doc_id)!=&doc) { return 2; }
        std::cout<<"D "<<doc.doc_id.value()<<' '<<hex(encoded.value())<<'\n';
    }
    const auto &rec=*reader.recommendation(); const auto &terms=rec.lexicon().terms();
    std::cout<<"R "<<rec.documents()<<' '<<rec.total_tokens()<<' '<<terms.size()<<'\n';
    for (std::size_t i=0;i<terms.size();++i) { std::cout<<"T "<<i<<' '<<hex(terms[i])<<' '<<rec.frequencies()[i]<<'\n'; }
    for (const auto &[scalar,ids]:rec.characters()) { std::cout<<"C "<<static_cast<std::uint32_t>(scalar);
        for (auto id:ids) { std::cout<<' '<<id.value(); } std::cout<<'\n'; }
}
