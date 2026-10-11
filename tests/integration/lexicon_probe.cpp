/*
 * PROJECT : SIFTWING
 * FILE    : lexicon_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 独立Python参考的hex词键/统计内存探针，输出不是生产持久化格式
 */
#include "siftwing/index/lexicon.h"
#include <iostream>
#include <stdexcept>

namespace {
std::uint64_t number(const char *raw) {
    const std::string value{raw}; std::size_t used=0;
    if (value.empty() || value[0]=='-') { throw std::invalid_argument("unsigned integer required"); }
    const auto n=std::stoull(value,&used);
    if (used!=value.size()) { throw std::invalid_argument("bad integer"); }
    return n;
}
unsigned digit(char c) {
    if(c>='0' && c<='9') { return static_cast<unsigned>(c-'0'); }
    if(c>='a' && c<='f') { return static_cast<unsigned>(c-'a'+10); }
    throw std::invalid_argument("bad hex");
}
std::string unhex(std::string_view value) {
    if(value.size()%2) { throw std::invalid_argument("odd hex"); }
    std::string out;
    for(std::size_t i=0;i<value.size();i+=2) { out+=static_cast<char>(digit(value[i])*16U+digit(value[i+1])); }
    return out;
}
std::string hex(std::string_view word) {
    constexpr char digits[]="0123456789abcdef"; std::string out;
    for(const unsigned char c:word) { out+=digits[c>>4U]; out+=digits[c&15U]; }
    return out;
}
void lexicon(const siftwing::index::Lexicon &lex) {
    std::cout<<"\"terms\":["; bool first=true;
    for(const auto &word:lex.terms()) {
        if(!first) { std::cout<<','; } first=false;
        std::cout<<"[\""<<hex(word)<<"\","<<lex.find(word)->value()<<']';
    }
    std::cout<<"],\"reverse\":["; first=true;
    for(std::size_t i=0;i<lex.terms().size();++i) {
        if(!first) { std::cout<<','; } first=false;
        std::cout<<'"'<<hex(*lex.term(siftwing::base::TermId::from_integer(i).value()))<<'"';
    }
    std::cout<<']';
}
template<typename T> int error(const siftwing::base::Result<T> &result) {
    std::cout<<"{\"error\":\""<<result.error().context<<"\"}\n"; return 1;
}
} // namespace
int main(int argc,char **argv) {
    try {
        using namespace siftwing::index;
        if(argc<2) { return 2; }
        const std::string mode{argv[1]};
        LexiconLimits limits{1000,1048576,4096,1000,1048576};
        RecommendationDictionaryLimits rlimits{limits,1000,1000000,100000,100000};
        if(mode=="lex") {
            if(argc!=2 && argc!=7) { return 2; }
            if(argc==7) { limits={number(argv[2]),number(argv[3]),number(argv[4]),number(argv[5]),number(argv[6])}; }
        } else if(mode=="rec") {
            if(argc!=4 && argc!=13) { return 2; }
            if(argc==13) { rlimits={{number(argv[4]),number(argv[5]),number(argv[6]),number(argv[7]),number(argv[8])},
                number(argv[9]),number(argv[10]),number(argv[11]),number(argv[12])}; }
        } else { return 2; }
        std::vector<std::string> words;
        siftwing::text::RecommendationFrequencies input;
        if(mode=="rec") { input.documents=number(argv[2]); input.total_tokens=number(argv[3]); }
        std::string line; std::size_t bytes=0;
        while(std::getline(std::cin,line)) {
            bytes+=line.size(); if(bytes>2097152 || words.size()>1000 || input.terms.size()>1000) { return 2; }
            if(mode=="lex") { words.push_back(unhex(line)); }
            else {
                const auto delimiter=line.find(' '); if(delimiter==line.npos) { return 2; }
                const auto count=number(line.substr(0,delimiter).c_str());
                if(!input.terms.emplace(unhex(std::string_view{line}.substr(delimiter+1)),count).second) { return 2; }
            }
        }
        if(!std::cin.eof()) { return 2; }
        if(mode=="lex") {
            std::vector<std::string_view> views; for(const auto &word:words) { views.push_back(word); }
            auto result=Lexicon::build(views,limits); if(!result) { return error(result); }
            std::cout<<'{'; lexicon(result.value()); std::cout<<"}\n";
        } else {
            auto result=RecommendationDictionary::build(input,rlimits); if(!result) { return error(result); }
            const auto &d=result.value(); std::cout<<'{'; lexicon(d.lexicon());
            std::cout<<",\"documents\":"<<d.documents()<<",\"total\":"<<d.total_tokens()<<",\"frequencies\":[";
            bool first=true; for(const auto count:d.frequencies()) { if(!first) { std::cout<<','; } first=false; std::cout<<count; }
            std::cout<<"],\"characters\":["; first=true;
            for(const auto &entry:d.characters()) {
                if(!first) { std::cout<<','; } first=false; std::cout<<'['<<static_cast<std::uint32_t>(entry.first)<<",[";
                bool start=true; for(auto id:entry.second) { if(!start) { std::cout<<','; } start=false; std::cout<<id.value(); }
                std::cout<<"]]";
            }
            std::cout<<"]}\n";
        }
        return std::cout?0:3;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 3; }
}
