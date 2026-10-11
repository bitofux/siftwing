/*
 * PROJECT : SIFTWING
 * FILE    : inverted_index_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 接受原创十六进制token批次并枚举索引供Python独立参考对照
 */
#include "siftwing/index/inverted_index.h"
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
std::string decode(const std::string &s) {
    if(s.size()%2) { throw std::runtime_error("odd hex length"); }
    std::string out;
    for(std::size_t i=0;i<s.size();i+=2) { out.push_back(static_cast<char>(std::stoul(s.substr(i,2),nullptr,16))); }
    return out;
}
std::string encode(std::string_view s) {
    std::ostringstream out; out<<std::hex<<std::setfill('0');
    for(unsigned char c:s) { out<<std::setw(2)<<static_cast<unsigned>(c); } return out.str();
}
}
int main() {
    try {
        using namespace siftwing;
        std::vector<index::IndexDocument> documents;
        std::string line;
        while(std::getline(std::cin,line)) {
            std::istringstream input(line); std::uint64_t id;
            if(!(input>>id)) { throw std::runtime_error("missing ID"); }
            text::DocumentTermFrequencies tf; std::string hex;
            while(input>>hex) { ++tf.terms[decode(hex)]; ++tf.total_tokens; }
            documents.push_back({base::DocumentId::from_integer(id).value(),std::move(tf)});
        }
        constexpr auto m=std::numeric_limits<std::uint64_t>::max();
        const auto index=index::InvertedIndex::build(documents,{{m,m,m,m,m},m,m,m,m}).value();
        const auto st=index.statistics(); std::cout<<"S "<<st.documents<<' '<<st.total_tokens<<' '<<st.total_postings<<'\n';
        for(const auto &d:index.documents()) { std::cout<<"D "<<d.document_id.value()<<' '<<d.total_tokens<<' '<<d.distinct_terms<<'\n'; }
        for(std::size_t n=0;n<index.lexicon().terms().size();++n) {
            const auto id=base::TermId::from_integer(n).value();
            std::cout<<"T "<<n<<' '<<encode(*index.lexicon().term(id))<<' '<<*index.document_frequency(id)<<'\n';
            for(const auto &p:*index.postings(id)) { std::cout<<"P "<<n<<' '<<p.document_id().value()<<' '<<p.term_frequency()<<'\n'; }
        }
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
