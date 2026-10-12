/*
 * PROJECT : SIFTWING
 * FILE    : candidates_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 将真实Query和倒排匹配结果导出给独立逐文档集合参考
 */
#include "siftwing/query/candidates.h"
#include "siftwing/text/english_tokenizer.h"
#include <iostream>
#include <limits>

int main() {
    using namespace siftwing;
    auto pipeline=text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
        text::StopWords::parse({}, {100,10,10,100}).value()).value();
    const auto huge=std::numeric_limits<std::uint64_t>::max();
    const query::QueryLimits ql{{{100000,100000},{100000,1000,1000,100000},
        {1000,100000,1000,1000,100000,1000,100000}},huge};
    unsigned mode=0;query::CandidateLimits budget;std::uint64_t k=0;std::size_t count=0;
    // 仅测试流式协议：每行一个完整集合；生产接口没有此序列化或CLI合同。
    while(std::cin>>mode>>budget.max_query_terms>>budget.max_postings>>budget.max_candidates>>k>>count) {
        std::vector<index::IndexDocument> documents;
        for(std::size_t i=0;i<count;++i) {
            std::uint64_t number=0;std::size_t terms=0;std::cin>>number>>terms;
            text::DocumentTermFrequencies tf;
            for(std::size_t j=0;j<terms;++j) {
                std::string word;std::uint64_t frequency=0;std::cin>>word>>frequency;
                tf.terms.emplace(word,frequency);tf.total_tokens+=frequency;
            }
            documents.push_back({base::DocumentId::from_integer(number).value(),std::move(tf)});
        }
        std::size_t terms=0;std::cin>>terms;std::string text;
        for(std::size_t i=0;i<terms;++i) { std::string word;std::cin>>word;text+=word+' '; }
        if(!std::cin) { return 2; }
        auto index=index::InvertedIndex::build(documents,
            {{100000,1000000,1000,10000,1000000},10000,huge,huge,10000}).value();
        const auto q=query::Query::from_text(text,*pipeline,
            {mode==0?query::QueryMode::all:query::QueryMode::any,k},ql).value();
        const auto result=query::collect_candidates(q,index,budget);
        if(!result) { std::cout<<"ERR "<<result.error().context<<'\n';continue; }
        std::cout<<"OK "<<result.value().size();
        for(auto id:result.value()) { std::cout<<' '<<id.value(); } std::cout<<'\n';
    }
    return std::cin.eof()?0:2;
}
