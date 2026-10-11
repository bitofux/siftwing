/*
 * PROJECT : SIFTWING
 * FILE    : weighted_index_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 手算TF-IDF/L2、零向量、全宽整数、移动接管、预算与规范字节导出
 */
#include "siftwing/index/weighted_index.h"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>

namespace {
using namespace siftwing;
int checks=0, failures=0;
void check(bool ok,std::string_view description) {++checks;if(!ok){++failures;std::cerr<<"FAIL: "<<description<<'\n';}}
bool close(double a,double b) {return std::abs(a-b)<=1e-12*std::max(std::abs(a),std::abs(b));}
auto did(std::uint64_t n){return base::DocumentId::from_integer(n).value();}
auto tid(std::uint64_t n){return base::TermId::from_integer(n).value();}
constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
index::WeightLimits limits(){return {maximum,maximum,maximum};}
auto raw(const std::vector<index::IndexDocument> &documents) {
    return index::InvertedIndex::build(documents,{{maximum,maximum,maximum,maximum,maximum},maximum,maximum,maximum,maximum}).value();
}
std::vector<index::IndexDocument> input() {return {{did(maximum),{{},0}},{did(7),{{{"a",1}},1}},{did(0),{{{"a",4},{"b",3}},7}}};}
auto weighted(const std::vector<index::IndexDocument> &documents){return index::WeightedIndex::build(raw(documents),limits()).value();}
template<typename T> void error(const base::Result<T> &v,std::string_view context){check(!v,"expected failure");if(!v){check(v.error().context==context && !v.error().message.empty(),context);}}
void empty(const index::WeightedIndex &v){check(v.raw_index().statistics().documents==0 && v.raw_index().lexicon().terms().empty() && !v.weights(tid(0)) && !v.inverse_document_frequency(tid(0)) && !v.document_norm(did(0)),"moved-from empty owned state");}
struct RoundingGuard{int old=std::fegetround();~RoundingGuard(){std::fesetround(old);}};
struct Punctuation : std::numpunct<char>{char do_decimal_point()const override{return ',';}char do_thousands_sep()const override{return '_';}std::string do_grouping()const override{return "\3";}};
struct LocaleGuard{std::locale old=std::locale();~LocaleGuard(){std::locale::global(old);}};
}
int main(){
    try{
        using namespace siftwing::index;
        const double a=1.0+std::log(4.0/3.0), b=1.0+std::log(4.0/2.0);
        // 独立手算用sqrt(平方和)，不镜像生产hypot迭代；空篇计N但不占关系。
        const double expected_norm=std::sqrt(16*a*a+9*b*b);
        auto source=raw(input()); auto v=WeightedIndex::build(std::move(source),limits()).value();
        check(source.statistics().documents==0 && source.lexicon().terms().empty(),"successful build transfers and clears source");
        check(v.raw_index().statistics().documents==3 && v.raw_index().statistics().total_postings==3,"raw N/DF/relations unchanged");
        check(v.raw_index().lexicon().terms()==std::vector<std::string>{"a","b"},"term identity unchanged");
        check(close(*v.inverse_document_frequency(tid(0)),a) && close(*v.inverse_document_frequency(tid(1)),b),"smoothed hand computed IDF");
        check(close(*v.document_norm(did(0)),expected_norm) && close(*v.document_norm(did(7)),a),"hand computed pre normalization norms");
        check(close((*v.weights(tid(0)))[0],4*a/expected_norm) && close((*v.weights(tid(1)))[0],3*b/expected_norm),"L2 normalized hand weights");
        check((*v.weights(tid(0)))[1]==1.0,"one term document unit vector");
        const auto zero=*v.document_norm(did(maximum)); check(zero==0.0 && !std::signbit(zero),"selected empty doc canonical positive zero norm");
        check(!v.weights(tid(2)) && !v.weights(tid(maximum)) && !v.inverse_document_frequency(tid(maximum)) && !v.document_norm(did(42)),"unknown IDs distinguished");
        check(v.raw_index().postings(tid(0))->front().term_frequency()==4 && v.raw_index().postings(tid(0))->at(1).term_frequency()==1,"integer raw TF preserved");
        auto one=weighted({{did(42),{{{"a",3},{"b",4}},7}}});
        check(*one.inverse_document_frequency(tid(0))==1.0 && *one.document_norm(did(42))==5.0 && close(one.weights(tid(0))->front(),0.6) && close(one.weights(tid(1))->front(),0.8),"shared term positive weight and 3-4-5 unit vector");
        auto high=weighted({{did(maximum),{{{"big",maximum-1},{"tiny",1}},maximum}}});
        check(high.raw_index().postings(tid(0))->front().term_frequency()==maximum-1,"full width raw counter never replaced by rounded double");
        check(high.weights(tid(0))->front()==1.0 && close(high.weights(tid(1))->front(),1.0/static_cast<double>(maximum-1)),"very small positive component survives normalization");
        for(const auto &pair:std::array<std::array<std::uint64_t,2>,7>{{{1,1},{7,7},{maximum,maximum},{maximum,maximum-1},{maximum,1},{9007199254740993ULL,7},{7,1}}}){
            const auto f=smoothed_idf(pair[0],pair[1]); check(f && std::isfinite(f.value()) && f.value()>=1.0,"IDF full uint64 range finite");
        }
        check(smoothed_idf(maximum,maximum).value()==1.0,"max integer plus one cannot wrap");
        for(const auto &pair:std::array<std::array<std::uint64_t,2>,4>{{{0,0},{0,1},{1,0},{1,2}}}){error(smoothed_idf(pair[0],pair[1]),"weights.statistics");}
        auto no=weighted({}); empty(no); check(no.serialize({maximum}).value()=="SIFTWING-WEIGHTS 1 1\nS 0 0 0\n","empty complete canonical serialization");
        auto all_empty=weighted({{did(0),{{},0}},{did(42),{{},0}}}); check(all_empty.raw_index().statistics().documents==2 && *all_empty.document_norm(did(42))==0.0 && !all_empty.weights(tid(0)),"all empty no division by zero");
        check(all_empty.serialize({maximum}).value().find("D 42 0 0 0000000000000000\n")!=std::string::npos,"zero bit serialization");
        const auto bytes=v.serialize({maximum}).value();
        auto reversed=input();std::reverse(reversed.begin(),reversed.end()); check(weighted(reversed).serialize({maximum}).value()==bytes,"input permutation byte determinism");
        check(v.serialize({static_cast<std::uint64_t>(bytes.size())}).value()==bytes,"exact serialization byte budget inclusive");
        error(v.serialize({0}),"weights.serialization_limits");
        for(std::uint64_t n=1;n<bytes.size();++n){error(v.serialize({n}),"weights.serialization_bytes");}
        {LocaleGuard guard;std::locale::global(std::locale(guard.old,new Punctuation));check(v.serialize({maximum}).value()==bytes,"locale independent integer and bit encoding");}
        const WeightLimits exact{3,2,3};check(static_cast<bool>(WeightedIndex::build(raw(input()),exact)),"three exact build budgets inclusive");
        for(std::size_t n=0;n<3;++n){auto l=exact;const std::array<std::uint64_t*,3> fields{&l.max_documents,&l.max_terms,&l.max_postings};--*fields[n];
            const std::array<std::string_view,3> contexts{"weights.documents","weights.terms","weights.postings"};
            auto owned=raw(input());error(WeightedIndex::build(std::move(owned),l),contexts[n]);check(owned.statistics().documents==0,"failure also consumes explicitly moved source");
            *fields[n]=0;error(WeightedIndex::build(raw({}),l),"weights.limits");}
        {RoundingGuard guard;
            for(int mode:std::array<int,3>{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){check(std::fesetround(mode)==0,"rounding mode set for contract test");
                error(smoothed_idf(7,2),"weights.rounding");error(WeightedIndex::build(raw(input()),limits()),"weights.rounding");
                check(v.serialize({maximum}).value()==bytes,"serialization does not perform floating arithmetic");}
        }
        check(std::fegetround()==FE_TONEAREST,"rounding restored");
        auto moved=std::move(v);empty(v);check(moved.serialize({maximum}).value()==bytes,"move constructor complete state");
        one=std::move(moved);empty(moved);check(one.serialize({maximum}).value()==bytes,"move assignment replaces old raw and arrays");
        auto *alias=&one;one=std::move(*alias);check(one.serialize({maximum}).value()==bytes,"self move preserves");
        const auto value=weighted(input()).document_norm(did(0)); check(value && close(*value,expected_norm),"norm value outlives temporary");
        const auto output=weighted(input()).serialize({maximum}).value();check(output==bytes,"serialized string owns bytes after owner destruction");
    }catch(const std::exception &e){++failures;std::cerr<<"unexpected: "<<e.what()<<'\n';}
    std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
