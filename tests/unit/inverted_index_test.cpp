/*
 * PROJECT : SIFTWING
 * FILE    : inverted_index_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 手算索引统计、确定性、预算、错误和拥有寿命，Release仍执行检查
 */
#include "siftwing/index/inverted_index.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
using namespace siftwing;
int checks = 0, failures = 0;
void check(bool ok, std::string_view description) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
auto did(std::uint64_t n) { return base::DocumentId::from_integer(n).value(); }
auto tid(std::uint64_t n) { return base::TermId::from_integer(n).value(); }
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
index::InvertedIndexLimits limits() { return {{maximum,maximum,maximum,maximum,maximum},maximum,maximum,maximum,maximum}; }
void error(const base::Result<index::InvertedIndex> &r, std::string_view context) {
    check(!r, "expected failure");
    if (!r) { check(r.error().context == context && !r.error().message.empty(), context); }
}
std::string describe(const index::InvertedIndex &v) {
    std::ostringstream s;
    const auto st = v.statistics(); s << st.documents << ':' << st.total_tokens << ':' << st.total_postings;
    for (const auto &d : v.documents()) { s << 'D' << d.document_id.value() << ':' << d.total_tokens << ':' << d.distinct_terms; }
    for (std::size_t i=0; i<v.lexicon().terms().size(); ++i) {
        const auto t=tid(static_cast<std::uint64_t>(i)); s << 'T' << i << ':' << *v.lexicon().term(t) << ':' << *v.document_frequency(t);
        for (const auto &p : *v.postings(t)) { s << 'P' << p.document_id().value() << ':' << p.term_frequency(); }
    }
    return s.str();
}
void empty(const index::InvertedIndex &v) {
    check(v.lexicon().terms().empty() && v.documents().empty() && !v.postings(tid(0)) && !v.document(did(0)), "empty owned state");
    const auto st=v.statistics(); check(st.documents==0 && st.total_tokens==0 && st.total_postings==0, "empty statistics");
}
} // namespace
int main() {
    try {
        using namespace siftwing::index;
        // 输入顺序与ID大小无关联；含空文档和完整最大ID，以独立手算核TF/DF/N。
        const std::vector<IndexDocument> input{
            {did(42),{{{"apple",3},{"pear",1}},4}}, {did(maximum),{{},0}},
            {did(7),{{{"apple",2},{"banana",1}},3}}, {did(0),{{{"banana",1}},1}}};
        const auto snapshot=input;
        auto v=InvertedIndex::build(input,limits()).value();
        check(v.lexicon().terms()==std::vector<std::string>{"apple","banana","pear"}, "exact union and fixed IDs");
        const auto st=v.statistics();
        check(st.documents==4 && st.total_tokens==8 && st.total_postings==5, "N includes empty; tokens differ from relations");
        check(v.documents().size()==4 && v.documents()[0].document_id==did(0) && v.documents()[3].document_id==did(maximum), "sparse sorted documents");
        check(v.document(did(maximum))->total_tokens==0 && v.document(did(maximum))->distinct_terms==0, "selected empty document present");
        check(v.document(did(42))->total_tokens==4 && v.document(did(42))->distinct_terms==2, "per document statistics");
        const auto &apple=*v.postings(tid(0));
        check(apple==PostingList{Posting::from_count(did(7),2).value(),Posting::from_count(did(42),3).value()}, "exact raw TF and sorted identity");
        check(v.document_frequency(tid(0))==2 && v.document_frequency(tid(1))==2 && v.document_frequency(tid(2))==1, "DF counts documents not tokens");
        check(!v.postings(tid(3)) && !v.postings(tid(maximum)) && !v.document_frequency(tid(maximum)) && !v.document(did(8)), "unknown IDs distinguished");
        for (std::uint64_t t=0;t<3;++t) { check(static_cast<bool>(validate_posting_list(*v.postings(tid(t)),{4})), "constructed list satisfies M1.20"); }
        for (std::size_t i=0;i<input.size();++i) {
            check(input[i].document_id==snapshot[i].document_id && input[i].frequencies.terms==snapshot[i].frequencies.terms && input[i].frequencies.total_tokens==snapshot[i].frequencies.total_tokens, "input unchanged");
        }
        auto shuffled=input;
        std::sort(shuffled.begin(),shuffled.end(),[](const auto&a,const auto&b){return a.document_id<b.document_id;});
        do { check(describe(InvertedIndex::build(shuffled,limits()).value())==describe(v), "all 24 input permutations identical"); }
        while(std::next_permutation(shuffled.begin(),shuffled.end(),[](const auto&a,const auto&b){return a.document_id<b.document_id;}));
        empty(InvertedIndex::build({},limits()).value());
        auto only_empty=InvertedIndex::build({{did(0),{{},0}},{did(maximum),{{},0}}},limits()).value();
        check(only_empty.statistics().documents==2 && only_empty.statistics().total_tokens==0 && only_empty.lexicon().terms().empty(), "all empty retains N");
        auto owned=[] { std::vector<IndexDocument> local{{did(5),{{{"中文",2}},2}}}; auto out=InvertedIndex::build(local,limits()).value(); local[0].frequencies.terms.clear(); return out; }();
        check(owned.lexicon().terms()==std::vector<std::string>{"中文"} && owned.postings(tid(0))->front().term_frequency()==2, "independent ownership after input mutation and destruction");
        const auto original=describe(v); auto moved=std::move(v); empty(v); check(describe(moved)==original,"move construction retains complete state");
        owned=std::move(moved); empty(moved); check(describe(owned)==original,"move assignment replaces all old state");
        auto *alias=&owned; owned=std::move(*alias); check(describe(owned)==original,"self move preserves");
        const auto value=InvertedIndex::build(input,limits()).value().statistics(); check(value.documents==4,"statistics value outlives temporary");
        // 九预算分别证明全正、含端点和单项超限，不用超大分配猜测OOM行为。
        InvertedIndexLimits exact{{5,26,6,3,15},4,4,8,2};
        check(static_cast<bool>(InvertedIndex::build(input,exact)),"all exact budgets inclusive");
        const std::array<std::string_view,9> contexts{"lexicon.input_terms","lexicon.input_bytes","lexicon.term_bytes","lexicon.distinct_terms","lexicon.vocabulary_bytes","index.documents","index.document_tokens","index.total_tokens","index.postings_per_term"};
        for(std::size_t i=0;i<9;++i) {
            auto l=exact; std::array<std::uint64_t*,9> fields{&l.lexicon.max_input_terms,&l.lexicon.max_input_bytes,&l.lexicon.max_term_bytes,&l.lexicon.max_distinct_terms,&l.lexicon.max_vocabulary_bytes,&l.max_documents,&l.max_document_tokens,&l.max_total_tokens,&l.max_postings_per_term};
            --*fields[i]; error(InvertedIndex::build(input,l),contexts[i]); *fields[i]=0;
            error(InvertedIndex::build({},l),"index.limits");
        }
        error(InvertedIndex::build({input[0],input[0]},limits()),"index.doc_id");
        error(InvertedIndex::build({{did(0),{{},0}},{did(0),{{{"a",1}},1}}},limits()),"index.doc_id");
        error(InvertedIndex::build({{did(0),{{{"a",0}},0}}},limits()),"index.tf");
        error(InvertedIndex::build({{did(0),{{{"a",1}},2}}},limits()),"index.tf");
        error(InvertedIndex::build({{did(0),{{},1}}},limits()),"index.tf");
        error(InvertedIndex::build({{did(0),{{{"a",maximum},{"b",1}},0}}},limits()),"index.overflow");
        error(InvertedIndex::build({{did(0),{{{"a",maximum}},maximum}},{did(1),{{{"a",1}},1}}},limits()),"index.overflow");
        auto max_tf=InvertedIndex::build({{did(maximum),{{{"a",maximum}},maximum}}},limits()).value();
        check(max_tf.postings(tid(0))->front().term_frequency()==maximum && max_tf.document_frequency(tid(0))==1 && max_tf.statistics().total_tokens==maximum,"full uint64 counts never converted to floating point");
        for(const auto &key:std::array<std::string,5>{"", "two words", "\xC0\xAF", std::string("a\0b",3),"a\xE3\x80\x80"}) {
            const auto r=InvertedIndex::build({{did(0),{{{key,1}},1}}},limits()); check(!r && r.error().context.rfind("lexicon.",0)==0,"bad key delegated to lexicon");
        }
        const auto literal=InvertedIndex::build({{did(0),{{{"A",1},{"a",2},{"!",1},{"\xEF\xBB\xBF",1}},5}}},limits()).value();
        check(literal.lexicon().terms().size()==4 && literal.lexicon().find("A")!=literal.lexicon().find("a"),"no second normalization or filtering");
        auto bad=input; bad.back().frequencies.total_tokens=2; const auto prior=bad;
        error(InvertedIndex::build(bad,limits()),"index.tf");
        for(std::size_t i=0;i<bad.size();++i) { check(bad[i].document_id==prior[i].document_id && bad[i].frequencies.terms==prior[i].frequencies.terms && bad[i].frequencies.total_tokens==prior[i].frequencies.total_tokens,"failed build preserves input"); }
    } catch(const std::exception &e) { ++failures; std::cerr<<"unexpected: "<<e.what()<<'\n'; }
    std::cout<<"checks="<<checks<<" failures="<<failures<<'\n'; return failures?1:0;
}
