/*
 * PROJECT : SIFTWING
 * FILE    : candidates_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 手写文档集合证明AND/OR、未知词、预算端点与拥有生命
 */
#include "siftwing/query/candidates.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool condition, const char *label) {
    ++checks; if (!condition) { throw std::runtime_error(label); }
}
base::DocumentId id(std::uint64_t value) { return base::DocumentId::from_integer(value).value(); }
query::CandidateDocuments ids(std::initializer_list<std::uint64_t> values) {
    query::CandidateDocuments result;
    for (auto value : values) { result.push_back(id(value)); } return result;
}
template <typename T> void error(const base::Result<T> &result, const char *context) {
    check(!result && result.error().context == context, context);
}
index::InvertedIndexLimits index_limits() {
    return {{1000,10000,100,1000,10000},100,10000,100000,100};
}
} // namespace
int main() {
    try {
        using namespace siftwing;
        using namespace query;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({}, {100,10,10,100}).value()).value();
        const QueryLimits ql{{{1000,1000},{1000,100,100,1000},
            {100,1000,100,100,1000,100,1000}},100};
        const auto q = [&](std::string_view text, QueryMode mode = QueryMode::any) {
            return Query::from_text(text,*pipeline,{mode,1},ql).value();
        };
        const std::vector<index::IndexDocument> input{{id(maximum),{{{"b",9},{"c",1}},10}},
            {id(5),{{{"a",1},{"b",2}},3}}, {id(0),{{{"a",7}},7}},
            {id(99),{{{"c",1}},1}}, {id(10),{{},0}}};
        auto index = index::InvertedIndex::build(input,index_limits()).value();
        const CandidateLimits budget{100,1000,100};
        // 独立手写黄金：a={0,5}、b={5,MAX}、c={99,MAX}；空文档10不命中。
        for (auto mode : {QueryMode::all, QueryMode::any}) {
            check(collect_candidates(q("a",mode),index,budget).value()==ids({0,5}),"single term");
            check(collect_candidates(q("a a A",mode),index,{1,2,2}).value()==ids({0,5}),"repeat is one constraint and one admitted list");
            check(collect_candidates(q("unknown",mode),index,{1,1,1}).value().empty(),"all unknown is successful empty");
        }
        const auto both = q("b a");
        check(both.top_k()==1 && collect_candidates(both,index,{2,4,3}).value()==ids({0,5,maximum}),"OR full unique ascending output ignores top_k");
        check(collect_candidates(q("a b",QueryMode::all),index,{2,4,1}).value()==ids({5}),"AND only final output counts against budget");
        check(collect_candidates(q("a c",QueryMode::all),index,{2,4,1}).value().empty(),"disjoint intersection");
        check(collect_candidates(q("a b c",QueryMode::all),index,{3,6,1}).value().empty(),"three-way intersection empty");
        check(collect_candidates(q("c b a"),index,{3,6,4}).value()==ids({0,5,99,maximum}),"three-way union with overlaps");
        check(collect_candidates(q("a unknown"),index,{2,2,2}).value()==ids({0,5}),"OR ignores unknown");
        check(collect_candidates(q("a unknown",QueryMode::all),index,{2,2,1}).value().empty(),"AND unknown blocks all matches");
        // 准入预算对完整已知列表计量，即使AND最终因未知或不相交而为空也不绕过。
        error(collect_candidates(q("a unknown",QueryMode::all),index,{2,1,1}),"candidate.postings");
        error(collect_candidates(q("a c",QueryMode::all),index,{2,3,1}),"candidate.postings");
        error(collect_candidates(both,index,{1,4,3}),"candidate.terms");
        error(collect_candidates(both,index,{2,3,3}),"candidate.postings");
        error(collect_candidates(both,index,{2,4,2}),"candidate.documents");
        for (unsigned slot=0;slot<3;++slot) {
            auto zero=budget;
            const std::array<std::uint64_t *,3> fields{{&zero.max_query_terms,&zero.max_postings,&zero.max_candidates}};
            *fields[slot]=0; error(collect_candidates(both,index,zero),"candidate.limits");
        }
        auto moved_query=q("a"); const auto destination=std::move(moved_query);
        error(collect_candidates(moved_query,index,budget),"candidate.query");
        error(collect_candidates(moved_query,index,{}),"candidate.limits");
        check(collect_candidates(destination,index,budget).value()==ids({0,5}),"moved destination remains valid");
        const auto empty=index::InvertedIndex::build({},index_limits()).value();
        check(collect_candidates(both,empty,budget).value().empty(),"empty index is successful empty");
        const auto before_terms=index.lexicon().terms();
        const auto before_postings=*index.postings(*index.lexicon().find("a"));
        const auto before_tf=both.frequencies();
        auto owned=collect_candidates(both,index,{maximum,maximum,maximum}).value();
        auto copy=owned;copy.clear();
        check(owned==ids({0,5,maximum}) && copy.empty(),"result owns independent copy");
        check(index.lexicon().terms()==before_terms && *index.postings(*index.lexicon().find("a"))==before_postings &&
            both.frequencies().terms==before_tf.terms && both.frequencies().total_tokens==before_tf.total_tokens,
            "success and failure preserve both inputs");
        const auto moved_index=std::move(index);
        check(collect_candidates(both,index,budget).value().empty(),"moved index has no vocabulary");
        check(collect_candidates(both,moved_index,budget).value()==owned,"index move preserves destination matches");
        pipeline.reset();
        check(collect_candidates(both,moved_index,budget).value()==owned,"candidate matching has no pipeline lifetime dependency");
        const auto escaped=[] {
            auto p=text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
                text::StopWords::parse({}, {100,10,10,100}).value()).value();
            const QueryLimits l{{{100,100},{100,10,10,100},{10,100,10,10,100,10,100}},10};
            const auto local_query=Query::from_text("a",*p,{},l).value();
            const auto local_index=index::InvertedIndex::build({{id(7),{{{"a",1}},1}}},index_limits()).value();
            return collect_candidates(local_query,local_index,{1,1,1}).value();
        }();
        check(escaped==ids({7}),"output survives query and index destruction");
        std::cout<<"candidate unit checks="<<checks<<" failures=0\n";return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
