/*
 * PROJECT : SIFTWING
 * FILE    : recommendation_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 手算推荐距离/三键排序、完整准入与拥有输出的行为合同
 */
#include "siftwing/query/recommendation.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing;
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
unsigned checks = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) { throw std::runtime_error(label); }
}
index::RecommendationDictionary dictionary(const text::FrequencyTable &table = {}) {
    text::RecommendationFrequencies frequencies;
    frequencies.documents = 1; frequencies.terms = table;
    for (const auto &entry : table) { frequencies.total_tokens += entry.second; }
    return index::RecommendationDictionary::build(frequencies,
        {{maximum,maximum,maximum,maximum,maximum},maximum,maximum,maximum,maximum}).value();
}
query::RecommendationLimits limits() { return {10000,10000,maximum,10000,1000,10000,1000000,10000}; }
void rejected(std::string_view key, const index::RecommendationDictionary &dict,
              std::uint64_t k, const query::RecommendationLimits &l, const char *context) {
    const auto r = query::recommend_terms(key, dict, k, l);
    check(!r && r.error().context == context && !r.error().message.empty(), context);
}
void expect(const query::RecommendedTerms &r, const std::vector<std::string> &terms,
            const std::vector<std::uint64_t> &distances,
            const index::RecommendationDictionary &dict) {
    check(r.size() == terms.size(), "exact prefix size");
    for (std::size_t i = 0; i < r.size(); ++i) {
        check(r[i].term == terms[i] && r[i].distance == distances[i], "hand computed word distance/order");
        check(r[i].term_id == *dict.lexicon().find(terms[i]) &&
            r[i].frequency == *dict.frequency(r[i].term_id), "fields from same recommendation dictionary");
    }
}
void behavior() {
    const auto d = dictionary({{"cat",2},{"bat",5},{"rat",5},{"act",9},{"dog",100}});
    const auto l = limits();
    // 插入/删除/替换各1；交换cat→act为两步，距离优先于频次，bat/rat同频用字节序。
    expect(query::recommend_terms("cat",d,maximum,l).value(),{"cat","bat","rat","act"},{0,1,1,2},d);
    for (std::uint64_t k : {1ULL,2ULL,3ULL,4ULL,5ULL}) {
        const auto r = query::recommend_terms("cat",d,k,l).value();
        check(r.size() == std::min<std::uint64_t>(k,4), "k returns exact min without padding");
        check(r.front().term == "cat", "exact match retained");
    }
    check(query::recommend_terms("XYZ",d,1,l).value().empty(), "no shared character no global scan");
    check(query::recommend_terms("cat",dictionary(),1,l).value().empty(), "empty dictionary success empty");
    const auto cn = dictionary({{"中国",2},{"中国人",4},{"中共",3},{"国中",10},{"a中",3},{"😀中",3}});
    expect(query::recommend_terms("中国",cn,maximum,l).value(),
        {"中国","中国人","中共","国中","a中","😀中"},{0,1,1,2,2,2},cn);
    const auto repeated = dictionary({{"aa",2},{"ab",4},{"a",3}});
    expect(query::recommend_terms("aa",repeated,maximum,l).value(),{"aa","ab","a"},{0,1,1},repeated);
    const auto insert = dictionary({{"ab",1}});
    expect(query::recommend_terms("a",insert,1,l).value(),{"ab"},{1},insert);
    expect(query::recommend_terms("abc",insert,1,l).value(),{"ab"},{1},insert);
    const auto unicode = dictionary({{"😀a",1},{"中a",2},{"ea",3},{"éa",4},{"éa",5},{"Aa",6}});
    expect(query::recommend_terms("😀a",unicode,maximum,l).value(),
        {"😀a","Aa","éa","ea","中a","éa"},{0,1,1,1,1,2},unicode);
    const auto exact_case = dictionary({{"A",1},{"a",2}});
    expect(query::recommend_terms("A",exact_case,1,l).value(),{"A"},{0},exact_case);
    const auto huge = dictionary({{"a",maximum}});
    check(query::recommend_terms("a",huge,maximum,l).value().front().frequency == maximum,
        "frequency full uint64 no signed or floating conversion");
    const auto close = dictionary({{"q",100},{"zzzzz",1}});
    expect(query::recommend_terms("zzq",close,1,l).value(),{"q"},{2},close);
    const auto restricted = dictionary({{"aqqqqqq",1},{"bb",100}});
    expect(query::recommend_terms("aa",restricted,1,l).value(),{"aqqqqqq"},{6},restricted);
    // 单词U+FEFF不是White_Space，组合标量不NFC；零宽连接符也按原标量参与。
    const auto format = dictionary({{"\xef\xbb\xbf",1},{"a\xe2\x80\x8d",2}});
    check(query::recommend_terms("\xef\xbb\xbf",format,1,l).value().front().distance == 0,"BOM key retained");
    check(query::recommend_terms("a\xe2\x80\x8d",format,1,l).value().front().distance == 0,"ZWJ key retained");
}
void budgets() {
    const auto d = dictionary({{"cat",2},{"bat",5},{"rat",5},{"act",9},{"dog",100}});
    using L = query::RecommendationLimits;
    const std::vector<std::uint64_t L::*> fields{&L::max_query_bytes,&L::max_query_scalars,&L::max_top_k,
        &L::max_character_links,&L::max_candidates,&L::max_candidate_bytes,&L::max_distance_cells,&L::max_output_bytes};
    for (auto field : fields) { auto l = limits(); l.*field = 0; rejected("",d,0,l,"recommend.limits"); }
    rejected("",d,0,limits(),"recommend.top_k");
    auto l = limits(); l.max_top_k = 1; rejected("",d,2,l,"recommend.top_k");
    l = limits(); l.max_query_bytes = 2; rejected("cat",d,1,l,"recommend.query_bytes");
    l = limits(); l.max_query_scalars = 2; rejected("cat",d,1,l,"recommend.query_scalars");
    l = limits(); l.max_character_links = 9; rejected("cat",d,1,l,"recommend.character_links");
    l = limits(); l.max_candidates = 3; rejected("cat",d,1,l,"recommend.candidates");
    l = limits(); l.max_candidate_bytes = 11; rejected("cat",d,1,l,"recommend.candidate_bytes");
    l = limits(); l.max_distance_cells = 35; rejected("cat",d,1,l,"recommend.work");
    l = limits(); l.max_output_bytes = 2; rejected("cat",d,1,l,"recommend.output_bytes");
    // 全部端点：a4+c2+t4=10关系，4候选/12字节，3*(3+3+3+3)=36单元。
    l = {3,3,3,10,4,12,36,9};
    check(query::recommend_terms("cat",d,3,l).value().size() == 3,"all budget endpoints inclusive");
    l.max_output_bytes = 3;
    check(query::recommend_terms("cat",d,1,l).value().size() == 1,"output bytes only selected prefix");
    l.max_query_bytes = 6; l.max_query_scalars = 6; l.max_distance_cells = 72; l.max_output_bytes = 12;
    check(query::recommend_terms("catcat",d,3,l).value().size() == 3,"repeated characters lists charged once");
    const auto cn = dictionary({{"中国",1},{"国中",2}});
    l = {6,2,2,4,2,12,8,6};
    check(query::recommend_terms("中国",cn,1,l).value().front().term == "中国","Chinese scalar/cell and byte budgets differ");
    l.max_distance_cells = 7; rejected("中国",cn,1,l,"recommend.work");
    l = limits(); l.max_candidates=1; l.max_candidate_bytes=1; l.max_distance_cells=1;
    check(query::recommend_terms("XYZ",d,maximum,l).value().empty(),"unknown chars charge zero applicable work");
    l.max_query_scalars=1; rejected("XYZ",dictionary(),1,l,"recommend.query_scalars");
}
void invalid_and_ownership() {
    auto d = dictionary({{"a",2},{"ab",1}});
    const auto l = limits();
    rejected("",d,1,l,"recommend.query");
    for (auto key : {std::string{"a b"},std::string{"\t"},std::string{"a\xe3\x80\x80"}}) {
        rejected(key,d,1,l,"recommend.query");
    }
    // 固定25个Unicode15.1 White_Space，不把Python额外空白分类混入生产词形。
    const std::vector<std::string> spaces{"\t","\n","\v","\f","\r"," ","\xc2\x85","\xc2\xa0","\xe1\x9a\x80",
        "\xe2\x80\x80","\xe2\x80\x81","\xe2\x80\x82","\xe2\x80\x83","\xe2\x80\x84","\xe2\x80\x85",
        "\xe2\x80\x86","\xe2\x80\x87","\xe2\x80\x88","\xe2\x80\x89","\xe2\x80\x8a","\xe2\x80\xa8",
        "\xe2\x80\xa9","\xe2\x80\xaf","\xe2\x81\x9f","\xe3\x80\x80"};
    for (const auto &s : spaces) { rejected("a"+s,d,1,l,"recommend.query"); }
    for (const auto &bad : {std::string{"\xff"},std::string{"\xc0\x80"},std::string{"\xed\xa0\x80"},
         std::string{"\xf4\x90\x80\x80"},std::string{"a\xe4\xb8"}}) {
        rejected(bad,d,1,l,"recommend.utf8");
    }
    rejected(std::string{"a\0",2},d,1,l,"recommend.nul");
    rejected(std::string{"\0\xff",2},d,1,l,"recommend.utf8");
    rejected(std::string{"\0 ",2},d,1,l,"recommend.nul");
    rejected(std::string{" \xff",2},d,1,l,"recommend.utf8");
    std::string key="a";
    auto owned=query::recommend_terms(key,d,maximum,l).value(); key="other";
    auto moved=std::move(d);
    check(query::recommend_terms("a",d,1,l).value().empty(),"moved dictionary success empty");
    auto copy=owned; copy.front().term="changed";
    check(owned.front().term=="a" && moved.lexicon().terms().front()=="a","output owns independent copy");
    const auto surviving=[] { const auto local=dictionary({{"中国",1}});
        return query::recommend_terms("中国",local,1,limits()).value(); }();
    check(surviving.front().term=="中国" && surviving.front().frequency==1,"output survives all inputs destruction");
    std::string long_key(1000,'a');
    auto short_limit=l; short_limit.max_query_scalars=999;
    rejected(long_key,moved,1,short_limit,"recommend.query_scalars");
    const auto long_result=query::recommend_terms(long_key,moved,1,l).value();
    check(long_result.front().term=="a" && long_result.front().distance==999,"long input rolling DP exact distance");
}
} // namespace
int main() {
    try { behavior(); budgets(); invalid_and_ownership();
        std::cout<<"recommendation checks="<<checks<<" failures=0\n"; return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
