/*
 * PROJECT : SIFTWING
 * FILE    : lexicon_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 用独立手写期望验证编号、标量关系、预算及拥有/移动合同，Release仍有效
 */
#include "siftwing/index/lexicon.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <utility>

namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
template <typename T> void error(const siftwing::base::Result<T> &result, const char *context) {
    check(!result && result.error().context == context, context);
}
auto id(std::uint64_t value) { return siftwing::base::TermId::from_integer(value).value(); }
std::string bytes(std::initializer_list<unsigned> values) {
    std::string output;
    for (auto value : values) { output += static_cast<char>(value); }
    return output;
}
} // namespace
int main() {
    try {
        using namespace siftwing::index;
        using siftwing::text::RecommendationFrequencies;
        const LexiconLimits limits{100,1000,100,100,1000};
        const RecommendationDictionaryLimits rec_limits{limits,100,1000,100,100};
        // 原创期望按字节排序；重复和物理输入顺序均不决定ID，大小写保持精确。
        std::vector<std::string_view> input{"😀a","中中","b","a","中中"};
        const std::vector<std::string> expected{"a","b","中中","😀a"};
        auto lex_result = Lexicon::build(input,limits);
        check(static_cast<bool>(lex_result),"valid mixed vocabulary");
        if (!lex_result) { return 1; }
        auto lex = std::move(lex_result).value();
        check(lex.terms() == expected,"explicit unsigned UTF8 ordering");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            check(lex.find(expected[i]) == id(i),"forward rank");
            check(lex.term(id(i)) == expected[i],"reverse rank");
        }
        check(!lex.find("A") && !lex.find("") && !lex.find(bytes({0xff})),"lookup is exact without query validation");
        check(!lex.term(id(4)) && !lex.term(id(std::numeric_limits<std::uint64_t>::max())),"unknown full width IDs");
        for (unsigned iteration = 0; iteration < 20; ++iteration) {
            std::rotate(input.begin(),input.begin()+1,input.end());
            auto permutation=Lexicon::build(input,limits).value();
            check(permutation.terms() == expected,"permutation and duplicate stable");
        }
        auto changed = Lexicon::build({"0","a","b"},limits).value();
        check(changed.find("a") == id(1) && lex.find("a") == id(0),"different vocabulary can shift local IDs");
        auto empty_lex=Lexicon::build({},limits).value();
        check(empty_lex.terms().empty(),"empty vocabulary valid");
        // 输入有重复：输入条数/字节和与唯一输出条数/字节分别计量。
        const LexiconLimits exact{5,19,6,4,13};
        check(static_cast<bool>(Lexicon::build(input,exact)),"all exact budgets inclusive");
        for (unsigned field = 0; field < 5; ++field) {
            auto smaller = exact;
            if (field == 0) { --smaller.max_input_terms; }
            if (field == 1) { --smaller.max_input_bytes; }
            if (field == 2) { --smaller.max_term_bytes; }
            if (field == 3) { --smaller.max_distinct_terms; }
            if (field == 4) { --smaller.max_vocabulary_bytes; }
            check(!Lexicon::build(input,smaller),"one below budget fails whole build");
            auto zero = limits;
            if (field == 0) { zero.max_input_terms=0; }
            if (field == 1) { zero.max_input_bytes=0; }
            if (field == 2) { zero.max_term_bytes=0; }
            if (field == 3) { zero.max_distinct_terms=0; }
            if (field == 4) { zero.max_vocabulary_bytes=0; }
            error(Lexicon::build({},zero),"lexicon.limits");
        }
        for (const auto &bad : std::vector<std::string>{bytes({0x80}),bytes({0xc0,0xaf}),bytes({0xe2,0x82}),
                bytes({0xed,0xa0,0x80}),bytes({0xf4,0x90,0x80,0x80}),bytes({0xff})}) {
            error(Lexicon::build({bad},limits),"lexicon.utf8");
        }
        const std::string nul{"a\0b",3};
        error(Lexicon::build({nul},limits),"lexicon.nul");
        error(Lexicon::build({"",bytes({0xff})},limits),"lexicon.utf8");
        error(Lexicon::build({""},limits),"lexicon.term");
        for (auto cp : {9U,10U,11U,12U,13U,32U,0x85U,0xa0U,0x1680U,0x2000U,0x2001U,0x2002U,
                0x2003U,0x2004U,0x2005U,0x2006U,0x2007U,0x2008U,0x2009U,0x200aU,
                0x2028U,0x2029U,0x202fU,0x205fU,0x3000U}) {
            // 测试自写编码器，不调用生产UTF8库获得预期。
            std::string word="x";
            if (cp < 128) { word += bytes({cp}); }
            else if (cp < 2048) { word += bytes({0xc0U|(cp>>6U),0x80U|(cp&63U)}); }
            else { word += bytes({0xe0U|(cp>>12U),0x80U|((cp>>6U)&63U),0x80U|(cp&63U)}); }
            error(Lexicon::build({word},limits),"lexicon.term");
        }
        auto literal=Lexicon::build({"A","a","é","é","#","\xEF\xBB\xBF"},limits).value();
        check(literal.terms().size()==6,
              "no case folding NFC punctuation or BOM filtering");
        auto owned=[](LexiconLimits v) {
            std::string word="独立"; return Lexicon::build({word},v).value();
        }(limits);
        check(owned.find("独立")==id(0),"lexicon owns after input destruction");
        auto moved=std::move(owned);
        check(owned.terms().empty() && !owned.find("独立") && moved.find("独立")==id(0),"move source empty");
        auto *same_lexicon=&moved; moved=std::move(*same_lexicon);
        check(moved.find("独立")==id(0),"self move preserves");
        lex=std::move(moved);
        check(lex.find("独立")==id(0) && moved.terms().empty(),"move assignment replaces owned vocabulary");
        RecommendationFrequencies frequencies{{{"a",2},{"b",1},{"中中",3},{"😀a",4}},10,2};
        auto result=RecommendationDictionary::build(frequencies,rec_limits);
        check(static_cast<bool>(result),"valid recommendation");
        if (!result) { return 1; }
        auto dictionary=std::move(result).value();
        check(dictionary.lexicon().terms()==expected && dictionary.frequencies()==std::vector<std::uint64_t>{2,1,3,4},
              "lexicon aligned positive frequencies");
        check(dictionary.documents()==2 && dictionary.total_tokens()==10,"recommendation metadata preserved");
        const CharacterIndex characters{{U'a',{id(0),id(3)}},{U'b',{id(1)}},{U'中',{id(2)}},{U'😀',{id(3)}}};
        check(dictionary.characters()==characters,"scalar index unique sorted references, frequency does not duplicate links");
        check(dictionary.frequency(id(0))==2 && !dictionary.frequency(id(4)) &&
              !dictionary.frequency(id(std::numeric_limits<std::uint64_t>::max())),"frequency query boundary");
        const RecommendationDictionaryLimits rec_exact{{4,13,6,4,13},2,10,4,5};
        check(static_cast<bool>(RecommendationDictionary::build(frequencies,rec_exact)),"nine exact budgets inclusive");
        for (unsigned field=0; field<9; ++field) {
            auto small=rec_exact;
            if (field==0) { --small.lexicon.max_input_terms; }
            if (field==1) { --small.lexicon.max_input_bytes; }
            if (field==2) { --small.lexicon.max_term_bytes; }
            if (field==3) { --small.lexicon.max_distinct_terms; }
            if (field==4) { --small.lexicon.max_vocabulary_bytes; }
            if (field==5) { --small.max_documents; }
            if (field==6) { --small.max_total_tokens; }
            if (field==7) { --small.max_characters; }
            if (field==8) { --small.max_character_links; }
            check(!RecommendationDictionary::build(frequencies,small),"one below nine budgets");
            auto zero=rec_limits;
            if (field==0) { zero.lexicon.max_input_terms=0; }
            if (field==1) { zero.lexicon.max_input_bytes=0; }
            if (field==2) { zero.lexicon.max_term_bytes=0; }
            if (field==3) { zero.lexicon.max_distinct_terms=0; }
            if (field==4) { zero.lexicon.max_vocabulary_bytes=0; }
            if (field==5) { zero.max_documents=0; }
            if (field==6) { zero.max_total_tokens=0; }
            if (field==7) { zero.max_characters=0; }
            if (field==8) { zero.max_character_links=0; }
            error(RecommendationDictionary::build({},zero),"dictionary.limits");
        }
        auto bad=frequencies; bad.documents=0;
        error(RecommendationDictionary::build(bad,rec_limits),"dictionary.table");
        bad=frequencies; ++bad.total_tokens;
        error(RecommendationDictionary::build(bad,rec_limits),"dictionary.table");
        bad=frequencies; bad.terms["a"]=0;
        error(RecommendationDictionary::build(bad,rec_limits),"dictionary.table");
        bad=frequencies; bad.terms["a b"]=1;
        error(RecommendationDictionary::build(bad,rec_limits),"lexicon.term");
        const auto maximum=std::numeric_limits<std::uint64_t>::max();
        auto unlimited=rec_limits; unlimited.max_documents=maximum; unlimited.max_total_tokens=maximum;
        check(RecommendationDictionary::build({{{"a",maximum}},maximum,maximum},unlimited).value().frequency(id(0))==maximum,
              "full width frequency and delivery metadata");
        error(RecommendationDictionary::build({{{"a",maximum},{"b",1}},maximum,1},unlimited),"dictionary.overflow");
        auto empty_dictionary=RecommendationDictionary::build({},rec_limits).value();
        check(empty_dictionary.characters().empty(),"zero deliveries empty table valid");
        check(RecommendationDictionary::build({{},0,2},rec_limits).value().documents()==2,"empty delivered documents retained");
        frequencies.terms.clear();
        check(dictionary.lexicon().terms()==expected && dictionary.characters()==characters,"dictionary owns after input mutation");
        auto transferred=std::move(dictionary);
        check(dictionary.lexicon().terms().empty() && dictionary.frequencies().empty() && dictionary.characters().empty() &&
              dictionary.documents()==0 && dictionary.total_tokens()==0,"dictionary move source valid empty");
        dictionary=std::move(transferred);
        auto *same_dictionary=&dictionary; dictionary=std::move(*same_dictionary);
        check(dictionary.characters()==characters && dictionary.total_tokens()==10 && transferred.characters().empty(),
              "dictionary assignment and self move");
        std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';
        return failures?1:0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
