/*
 * PROJECT : SIFTWING
 * FILE    : top_k_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 导出稳定top-k结果供独立数学前缀参考核对
 */
#include "siftwing/query/top_k.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <iomanip>
#include <iostream>
#include <limits>

int main() {
    using namespace siftwing;
    auto pipeline=text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
        text::StopWords::parse({}, {100,10,10,100}).value()).value();
    const auto huge=std::numeric_limits<std::uint64_t>::max();
    const query::QueryLimits ql{{{100000,100000},{100000,1000,1000,100000},
        {1000,100000,1000,1000,100000,1000,100000}},huge};
    unsigned mode=0;
    query::TopKLimits budget;
    std::uint64_t k=0;
    std::size_t count=0;
    // 仅测试协议：输入原创整数TF，输出round-trip精度分数；不成为生产CLI。
    std::cout<<std::setprecision(std::numeric_limits<double>::max_digits10);
    while(std::cin>>mode>>budget.ranking.candidates.max_query_terms>>budget.ranking.candidates.max_postings
        >>budget.ranking.candidates.max_candidates>>budget.ranking.max_score_pairs>>budget.max_top_k>>k>>count) {
        std::vector<index::IndexDocument> documents;
        for(std::size_t i=0;i<count;++i) {
            std::uint64_t number=0;
            std::size_t terms=0;
            std::cin>>number>>terms;
            text::DocumentTermFrequencies tf;
            for(std::size_t j=0;j<terms;++j) {
                std::string word;
                std::uint64_t frequency=0;
                std::cin>>word>>frequency;
                tf.terms.emplace(word,frequency);
                tf.total_tokens+=frequency;
            }
            documents.push_back({base::DocumentId::from_integer(number).value(),std::move(tf)});
        }
        std::size_t terms=0;
        std::cin>>terms;
        std::string text;
        for(std::size_t i=0;i<terms;++i) { std::string word;std::cin>>word;text+=word+' '; }
        if(!std::cin) { return 2; }
        auto raw=index::InvertedIndex::build(documents,
            {{100000,1000000,1000,10000,1000000},10000,huge,huge,10000});
        if(!raw) { std::cerr<<raw.error().context<<": "<<raw.error().message<<'\n';return 2; }
        const auto weighted=index::WeightedIndex::build(std::move(raw).value(),{10000,10000,huge}).value();
        const auto q=query::Query::from_text(text,*pipeline,
            {mode==0?query::QueryMode::all:query::QueryMode::any,k},ql).value();
        const auto result=query::rank_top_k(q,weighted,budget);
        if(!result) { std::cout<<"ERR "<<result.error().context<<'\n';continue; }
        std::cout<<"OK "<<result.value().size();
        for(const auto &document:result.value()) {
            std::cout<<' '<<document.document_id.value()<<' '<<document.score;
        }
        std::cout<<'\n';
    }
    return std::cin.eof()?0:2;
}
