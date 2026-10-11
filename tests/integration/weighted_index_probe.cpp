/*
 * PROJECT : SIFTWING
 * FILE    : weighted_index_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 原创显式TF输入到原始/加权索引的规范字节导出，供独立Decimal参考核验
 */
#include "siftwing/index/weighted_index.h"
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
std::string decode(std::string_view hex){
    if(hex.size()%2){throw std::runtime_error("odd hex length");}
    std::string result;
    for(std::size_t i=0;i<hex.size();i+=2){result.push_back(static_cast<char>(std::stoul(std::string(hex.substr(i,2)),nullptr,16)));}
    return result;
}
}
int main(int argc,char **argv){
    try{
        using namespace siftwing;
        if(argc==4 && std::string_view(argv[1])=="--idf"){
            const auto value=index::smoothed_idf(std::stoull(argv[2]),std::stoull(argv[3])).value();
            std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));
            std::cout<<std::hex<<std::setfill('0')<<std::setw(16)<<bits<<'\n';return 0;
        }
        if(argc!=1){throw std::runtime_error("unexpected arguments");}
        constexpr auto m=std::numeric_limits<std::uint64_t>::max();
        std::vector<index::IndexDocument> input;std::string line;
        while(std::getline(std::cin,line)){
            std::istringstream row(line);std::uint64_t id;if(!(row>>id)){throw std::runtime_error("missing ID");}
            text::DocumentTermFrequencies tf;std::string term;
            while(row>>term){const auto colon=term.find(':');if(colon==term.npos){throw std::runtime_error("missing count");}
                auto key=decode(std::string_view(term).substr(0,colon));const auto count=static_cast<std::uint64_t>(std::stoull(term.substr(colon+1)));
                if(!tf.terms.emplace(std::move(key),count).second){throw std::runtime_error("duplicate key in probe");}
                tf.total_tokens=base::checked_add(tf.total_tokens,count).value();
            }
            input.push_back({base::DocumentId::from_integer(id).value(),std::move(tf)});
        }
        auto raw=index::InvertedIndex::build(input,{{m,m,m,m,m},m,m,m,m}).value();
        const auto weighted=index::WeightedIndex::build(std::move(raw),{m,m,m}).value();
        std::cout<<weighted.serialize({m}).value();return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
