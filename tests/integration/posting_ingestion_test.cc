/*
 * PROJECT : SIFTWING
 * FILE    : posting_ingestion_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 将显式选定文档的M1.13词频接成单词关系，核对原始TF与推荐累计口径
 */
#include "siftwing/index/posting.hpp"
#include "siftwing/text/term_frequency.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {
int checks=0;
int failures=0;
void check(bool condition, std::string_view description) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
} // namespace
int main() {
    try {
        using namespace siftwing;
        const text::TokenAnalysisLimits limits{20,200,20,20,200,20,200};
        const auto stopwords=text::StopWords::parse({}, {20,200,20,200}).value();
        auto a=text::analyze_tokens({"apple","banana","apple"}, stopwords, limits).value();
        auto b=text::analyze_tokens({"apple","apple","pear","apple"}, stopwords, limits).value();
        auto c=text::analyze_tokens({"banana"}, stopwords, limits).value();
        // 手算apple：两个显式搜索文档次数分别2/3；无apple的第三篇不产生零TF占位。
        // 此处只接一个指定词，不扫描语料建立倒排，也不从推荐累计回填单文档TF。
        index::PostingList apple{
            index::Posting::from_count(base::DocumentId::from_integer(7).value(), a.tf.terms.at("apple")).value(),
            index::Posting::from_count(base::DocumentId::from_integer(42).value(), b.tf.terms.at("apple")).value()};
        check(static_cast<bool>(index::validate_posting_list(apple,{2})), "explicit selected single-term list");
        check(apple.size()==2 && apple[0].document_id().value()==7 && apple[1].document_id().value()==42,
              "document membership distinct from token count");
        check(apple[0].term_frequency()==2 && apple[1].term_frequency()==3, "raw per-document occurrence counts");
        check(c.tf.terms.find("apple")==c.tf.terms.end(), "absent term omitted upstream");
        const text::RecommendationLimits rec_limits{10,20,20,200,100};
        const auto first=text::accumulate_recommendation({}, a.tf, rec_limits).value();
        const auto second=text::accumulate_recommendation(first, b.tf, rec_limits).value();
        const auto repeated=text::accumulate_recommendation(second, a.tf, rec_limits).value();
        check(repeated.terms.at("apple")==7 && repeated.documents==3, "recommendation repeats independently");
        check(apple[0].term_frequency()==2 && apple[1].term_frequency()==3 && apple.size()==2,
              "recommendation sum neither overwrites TF nor adds document relations");
    } catch (const std::exception& exception) {
        ++failures; std::cerr << "unexpected: " << exception.what() << '\n';
    }
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures==0 ? 0 : 1;
}
