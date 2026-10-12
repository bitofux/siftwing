/*
 * PROJECT : SIFTWING
 * FILE    : ranking_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 手算余弦、精确同分次序、完整输出、预算优先级与拥有生命
 */
#include "siftwing/query/ranking.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
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
index::InvertedIndexLimits il() {
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    return {{1000,10000,100,1000,10000},100,maximum,maximum,100};
}
index::WeightedIndex build(const std::vector<index::IndexDocument> &docs) {
    auto raw=index::InvertedIndex::build(docs,il());
    if(!raw) { throw std::runtime_error(raw.error().context+": "+raw.error().message); }
    auto weighted=index::WeightedIndex::build(std::move(raw).value(),{100,1000,10000});
    if(!weighted) { throw std::runtime_error(weighted.error().context+": "+weighted.error().message); }
    return std::move(weighted).value();
}
template<typename T> void error(const base::Result<T> &r, const char *context) {
    check(!r && r.error().context == context,context);
}
void near(double actual, double expected, const char *label) {
    check(std::isfinite(actual) && std::abs(actual-expected) <= 2e-14,label);
}
bool same(const query::RankedDocuments &a, const query::RankedDocuments &b) {
    if (a.size()!=b.size()) { return false; }
    for (std::size_t i=0;i<a.size();++i) {
        if (a[i].document_id!=b[i].document_id || a[i].score!=b[i].score) { return false; }
    }
    return true;
}
// 浮点环境属于调用线程；测试失败/异常也恢复原模式，不能污染后续检查。
struct RoundingGuard final {
    int original = std::fegetround();
    ~RoundingGuard() { (void)std::fesetround(original); }
};
} // namespace

int main() {
    try {
        using namespace siftwing;
        using namespace query;
        RoundingGuard guard;
        check(std::fesetround(FE_TONEAREST)==0,"set nearest");
        auto pipeline=text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({}, {100,10,10,100}).value()).value();
        const auto maximum=std::numeric_limits<std::uint64_t>::max();
        const QueryLimits ql{{{10000,10000},{10000,1000,1000,10000},
            {1000,10000,1000,1000,10000,1000,10000}},maximum};
        const auto q=[&](std::string_view text, QueryMode mode=QueryMode::any, std::uint64_t k=1) {
            return Query::from_text(text,*pipeline,{mode,k},ql).value();
        };
        const RankingLimits budget{{100,1000,100},10000};
        // N=DF=1使IDF=1，a:3/b:4的文档单位向量为(.6,.8)，不从生产权重读期望。
        const auto triangle=build({{id(7),{{{"a",3},{"b",4}},7}}});
        for (auto mode:{QueryMode::all,QueryMode::any}) {
            near(rank_candidates(q("a",mode),triangle,budget).value()[0].score,.6,"3-4-5 a score");
            near(rank_candidates(q("b",mode),triangle,budget).value()[0].score,.8,"3-4-5 b score");
            near(rank_candidates(q("b a b a b a b",mode),triangle,budget).value()[0].score,1,"matching direction cosine one");
        }
        near(rank_candidates(q("a unknown unknown"),triangle,budget).value()[0].score,.6,"unknown OR not in norm");
        near(rank_candidates(q("a a a a"),triangle,budget).value()[0].score,.6,"single dimension TF scaling cancels");
        near(rank_candidates(q("a b"),triangle,budget).value()[0].score,1.4/std::sqrt(2.0),"two-dimension unit query");
        check(rank_candidates(q("a unknown",QueryMode::all),triangle,budget).value().empty(),"AND unknown empty");
        check(rank_candidates(q("unknown"),triangle,budget).value().empty(),"all unknown empty");
        // 三篇共享同词表和DF；单位方向独立手写，查询重复TF确实改变排序。
        const std::vector<index::IndexDocument> docs{{id(maximum),{{{"a",1},{"b",3}},4}},
            {id(99),{{{"a",3},{"b",1}},4}},{id(0),{{{"a",3},{"b",1}},4}}};
        auto index=build(docs);
        const auto query=q("a b");
        const auto before=index.serialize({100000}).value();
        const auto tf=query.frequencies();
        auto ranked=rank_candidates(query,index,{{2,6,3},6}).value();
        check(query.top_k()==1 && ranked.size()==3,"full ranking ignores top_k");
        check(ranked[0].document_id==id(0) && ranked[1].document_id==id(99) && ranked[2].document_id==id(maximum),"exact ties use DocId ascending");
        check(ranked[0].score==ranked[1].score && ranked[1].score==ranked[2].score,"symmetric directions exact tie");
        near(ranked[0].score,4/std::sqrt(20.0),"shared DF cosine independent formula");
        const auto repeated=rank_candidates(q("b b b a"),index,budget).value();
        check(repeated.front().document_id==id(maximum),"repeat TF changes direction and winner");
        near(repeated.front().score,1,"matching repeated direction");
        near(repeated.back().score,.6,"opposite repeated direction");
        auto reversed=docs;
        std::reverse(reversed.begin(),reversed.end());
        const auto reordered=build(reversed);
        check(same(rank_candidates(q("b a"),reordered,budget).value(),ranked),"input and query order preserve exact output");
        check(same(rank_candidates(q("a b",QueryMode::all,maximum),index,budget).value(),ranked),"AND and top_k endpoint preserve scores");
        // 非查询词必须参与文档范数；空篇也计N，故这里的IDF不同于单篇黄金。
        const auto sparse=build({{id(0),{{{"a",1}},1}},{id(maximum),{{{"a",1},{"b",1}},2}},{id(10),{{},0}}});
        const auto sr=rank_candidates(q("a"),sparse,budget).value();
        check(sr.size()==2 && sr[0].document_id==id(0),"empty document not a match and unqueried dimensions penalize");
        near(sr[0].score,1,"single-term document one");
        const double ia=1+std::log(4.0/3.0), ib=1+std::log(2.0);
        near(sr[1].score,ia/std::sqrt(ia*ia+ib*ib),"global N DF smoothing and full document norm");
        // 两分数差小于常见epsilon；严格按实际double值排序，不能合并为身份顺序。
        const auto close=build({{id(0),{{{"a",1000000000000ULL},{"b",1000000000000ULL}},2000000000000ULL}},
            {id(maximum),{{{"a",1000000000000ULL},{"b",999999999999ULL}},1999999999999ULL}}});
        const auto cr=rank_candidates(q("a"),close,budget).value();
        check(cr.front().document_id==id(maximum) && cr[0].score>cr[1].score && cr[0].score-cr[1].score<1e-12,"near scores are not epsilon ties");
        // 配置、候选准入、评分准入的优先级和含端点预算。
        for (unsigned slot=0;slot<4;++slot) {
            auto zero=budget;
            const std::array<std::uint64_t *,4> fields{{&zero.candidates.max_query_terms,
                &zero.candidates.max_postings,&zero.candidates.max_candidates,&zero.max_score_pairs}};
            *fields[slot]=0;
            error(rank_candidates(q("unknown"),index,zero),"ranking.limits");
        }
        error(rank_candidates(query,index,{{1,6,3},1}),"candidate.terms");
        error(rank_candidates(query,index,{{2,5,3},1}),"candidate.postings");
        error(rank_candidates(query,index,{{2,6,2},1}),"candidate.documents");
        error(rank_candidates(query,index,{{2,6,3},5}),"ranking.work");
        error(rank_candidates(q("a unknown",QueryMode::all),index,{{2,2,1},1}),"candidate.postings");
        check(rank_candidates(q("a unknown",QueryMode::all),index,{{2,3,1},1}).value().empty(),"empty AND does not consume score budget");
        check(rank_candidates(q("a unknown unknown"),index,{{2,3,3},3}).value().size()==3,"work counts only known unique words");
        // 缺失维度也计C*T：OR为两个候选乘两个已知词，即使只有两条真实关系也需4。
        const auto disjoint=build({{id(0),{{{"a",1}},1}},{id(99),{{{"b",1}},1}}});
        error(rank_candidates(query,disjoint,{{2,2,2},3}),"ranking.work");
        check(rank_candidates(query,disjoint,{{2,2,2},4}).value().size()==2,"score-pair budget includes absent relationships");
        for (auto mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            check(std::fesetround(mode)==0,"set alternate rounding");
            error(rank_candidates(q("unknown"),index,budget),"ranking.rounding");
            error(rank_candidates(query,index,{}),"ranking.limits");
            check(std::fegetround()==mode,"ranking never changes rounding");
        }
        check(std::fesetround(FE_TONEAREST)==0,"restore nearest");
        auto moved=q("a"); const auto destination=std::move(moved);
        error(rank_candidates(moved,index,budget),"candidate.query");
        check(!rank_candidates(destination,index,budget).value().empty(),"moved query destination works");
        const auto empty=build({});
        check(rank_candidates(query,empty,budget).value().empty(),"empty index success");
        const auto all_empty=build({{id(0),{{},0}}});
        check(rank_candidates(query,all_empty,budget).value().empty(),"all-empty documents success");
        auto copy=ranked;copy.clear();
        check(copy.empty() && ranked.size()==3,"result independent copy");
        check(index.serialize({100000}).value()==before && query.frequencies().terms==tf.terms &&
            query.frequencies().total_tokens==tf.total_tokens,"success and failures preserve input bytes and TF");
        const auto moved_index=std::move(index);
        check(rank_candidates(query,index,budget).value().empty(),"moved index source empty");
        check(same(rank_candidates(query,moved_index,budget).value(),ranked),"moved index destination preserves output");
        pipeline.reset();
        check(same(rank_candidates(query,moved_index,budget).value(),ranked),"query independent of pipeline lifetime");
        const auto escaped=[] {
            auto p=text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
                text::StopWords::parse({}, {100,10,10,100}).value()).value();
            const QueryLimits l{{{100,100},{100,10,10,100},{10,100,10,10,100,10,100}},10};
            const auto local=Query::from_text("a",*p,{},l).value();
            const auto wi=build({{id(7),{{{"a",1}},1}}});
            return rank_candidates(local,wi,{{1,1,1},1}).value();
        }();
        check(escaped.size()==1 && escaped[0].document_id==id(7) && escaped[0].score==1,"output survives all owners");
        std::cout<<"ranking unit checks="<<checks<<" failures=0\n";
        return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
