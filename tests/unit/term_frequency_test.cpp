/*
 * PROJECT : SIFTWING
 * FILE    : term_frequency_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 手写重复频次/精确过滤/预算/溢出和纯函数累计的Release有效行为验收
 */
#include "siftwing/text/term_frequency.h"
#include <array>
#include <iostream>
#include <limits>
namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
using namespace siftwing::text;
const StopWordLimits stop_limits{10000, 1000, 1000, 10000};
const TokenAnalysisLimits token_limits{1000, 10000, 1000, 1000, 10000, 1000, 10000};
const RecommendationLimits recommendation_limits{1000, 1000, 1000, 10000, 10000};
template <class T> void failure(const siftwing::base::Result<T> &r, const char *context) {
    check(!r && r.error().context == context, "diagnostic context");
    if (!r) {
        bool threw = false;
        try {
            static_cast<void>(r.value());
        } catch (const std::bad_variant_access &) {
            threw = true;
        }
        check(threw, "failure publishes no payload");
    }
}
} // namespace
int main() {
    try {
        auto stops =
            StopWords::parse({"\xef\xbb\xbf THE\r\n的\nTHE\n \t\n#\na's\n", " the\n"}, stop_limits);
        check(stops && stops.value().entries() ==
                           std::set<std::string, std::less<>>{"#", "a's", "the", "的"},
              "normalization/duplicates/BOM/CRLF/literal punctuation");
        check(stops.value().contains("the") && !stops.value().contains("THE") &&
                  !stops.value().contains("a"),
              "exact case-sensitive matching/no retokenization");
        auto empty = StopWords::parse({}, stop_limits);
        check(empty && empty.value().entries().empty(), "empty sources allowed");
        auto blank = StopWords::parse({" \t\r\n　\n"}, stop_limits);
        check(blank && blank.value().entries().empty(), "all whitespace empty config");
        auto mid = StopWords::parse({"x\xef\xbb\xbf\n"}, stop_limits);
        check(mid && mid.value().contains("x\xef\xbb\xbf"), "interior BOM literal");
        const TokenSequence input{"the", "中国", "中国", "的", "a1", "THE"};
        auto analysis = analyze_tokens(input, stops.value(), token_limits);
        const FrequencyTable wanted{{"THE", 1}, {"a1", 1}, {"中国", 2}};
        check(analysis && analysis.value().tokens == TokenSequence{"中国", "中国", "a1", "THE"} &&
                  analysis.value().tf.terms == wanted && analysis.value().tf.total_tokens == 4,
              "owned sequence and hand-counted TF preserve repetitions");
        check(input == TokenSequence{"the", "中国", "中国", "的", "a1", "THE"}, "source unchanged");
        auto all = analyze_tokens({"the", "的"}, stops.value(), token_limits);
        check(all && all.value().tokens.empty() && all.value().tf.terms.empty() &&
                  all.value().tf.total_tokens == 0,
              "all filtered document");
        auto nil = analyze_tokens({}, empty.value(), token_limits);
        check(nil && nil.value().tokens.empty(), "empty document");
        auto accumulated =
            accumulate_recommendation({}, analysis.value().tf, recommendation_limits);
        check(accumulated && accumulated.value().terms == wanted &&
                  accumulated.value().documents == 1 && accumulated.value().total_tokens == 4,
              "first recommendation document");
        auto again = accumulate_recommendation(accumulated.value(), analysis.value().tf,
                                               recommendation_limits);
        check(again && again.value().terms == FrequencyTable{{"THE", 2}, {"a1", 2}, {"中国", 4}} &&
                  again.value().documents == 2 && again.value().total_tokens == 8,
              "repeated delivery counts actual occurrences, no implicit dedup/DF");
        auto plus_empty = accumulate_recommendation(again.value(), {}, recommendation_limits);
        check(plus_empty && plus_empty.value().documents == 3 &&
                  plus_empty.value().total_tokens == 8,
              "empty document counts as an explicit delivery");
        // 词条和token规范化职责不同；配置小写不暗中改变已经产生的token。
        auto case_check = analyze_tokens({"the", "THE"}, stops.value(), token_limits);
        check(case_check && case_check.value().tokens == TokenSequence{"THE"},
              "no hidden token normalization");
        std::string raw = " OWN \n";
        auto owned = StopWords::parse({raw}, stop_limits);
        raw = "gone";
        check(owned.value().contains("own"), "stopword bytes owned");
        TokenSequence borrowed{"own", "keep", "keep"};
        auto result = analyze_tokens(borrowed, owned.value(), token_limits);
        borrowed.clear();
        check(result && result.value().tokens == TokenSequence{"keep", "keep"} &&
                  result.value().tf.terms.at("keep") == 2,
              "output owns independent strings");
        // 每项预算精确等于合法，少一独立拒绝；词表字节只计唯一键，输出计重复。
        const std::string source = "a\n中国\n";
        const StopWordLimits exact_stop{source.size(), 2, 6, 7};
        check(static_cast<bool>(StopWords::parse({source}, exact_stop)), "equal stop limits");
        const std::array<std::uint64_t StopWordLimits::*, 4> sm{
            &StopWordLimits::max_input_bytes, &StopWordLimits::max_entries,
            &StopWordLimits::max_entry_bytes, &StopWordLimits::max_vocabulary_bytes};
        const std::array<const char *, 4> sc{"stopwords.input_bytes", "stopwords.entries",
                                             "stopwords.entry_bytes", "stopwords.vocabulary_bytes"};
        for (std::size_t i = 0; i < sm.size(); ++i) {
            auto l = exact_stop;
            --(l.*sm[i]);
            failure(StopWords::parse({source}, l), sc[i]);
            l = exact_stop;
            l.*sm[i] = 0;
            failure(StopWords::parse({}, l), "stopwords.limits");
        }
        const TokenSequence boundary{"中国", "a", "中国"};
        const TokenAnalysisLimits exact_tokens{3, 13, 6, 3, 13, 2, 7};
        check(static_cast<bool>(analyze_tokens(boundary, empty.value(), exact_tokens)),
              "equal analysis limits");
        const std::array<std::uint64_t TokenAnalysisLimits::*, 7> tm{
            &TokenAnalysisLimits::max_input_tokens,    &TokenAnalysisLimits::max_input_bytes,
            &TokenAnalysisLimits::max_token_bytes,     &TokenAnalysisLimits::max_output_tokens,
            &TokenAnalysisLimits::max_output_bytes,    &TokenAnalysisLimits::max_distinct_terms,
            &TokenAnalysisLimits::max_vocabulary_bytes};
        const std::array<const char *, 7> tc{
            "frequency.input_tokens",    "frequency.input_bytes",  "frequency.token_bytes",
            "frequency.output_tokens",   "frequency.output_bytes", "frequency.distinct_terms",
            "frequency.vocabulary_bytes"};
        for (std::size_t i = 0; i < tm.size(); ++i) {
            auto l = exact_tokens;
            --(l.*tm[i]);
            failure(analyze_tokens(boundary, empty.value(), l), tc[i]);
            l = exact_tokens;
            l.*tm[i] = 0;
            failure(analyze_tokens({}, empty.value(), l), "frequency.limits");
        }
        const DocumentTermFrequencies doc{{{"a", 1}, {"中国", 2}}, 3};
        const RecommendationLimits exact_rec{1, 2, 6, 7, 3};
        check(static_cast<bool>(accumulate_recommendation({}, doc, exact_rec)),
              "equal recommendation limits");
        const std::array<std::uint64_t RecommendationLimits::*, 5> rm{
            &RecommendationLimits::max_documents, &RecommendationLimits::max_distinct_terms,
            &RecommendationLimits::max_term_bytes, &RecommendationLimits::max_vocabulary_bytes,
            &RecommendationLimits::max_total_tokens};
        const std::array<const char *, 5> rc{
            "recommendation.documents", "recommendation.distinct_terms",
            "recommendation.term_bytes", "recommendation.vocabulary_bytes",
            "recommendation.total_tokens"};
        for (std::size_t i = 0; i < rm.size(); ++i) {
            auto l = exact_rec;
            if (i == 0) {
                failure(accumulate_recommendation(
                            accumulate_recommendation({}, doc, exact_rec).value(), {}, l),
                        rc[i]);
            } else {
                --(l.*rm[i]);
                failure(accumulate_recommendation({}, doc, l), rc[i]);
            }
            l = exact_rec;
            l.*rm[i] = 0;
            failure(accumulate_recommendation({}, doc, l), "recommendation.limits");
        }
        failure(StopWords::parse({"a b\n"}, stop_limits), "stopwords.entry");
        failure(StopWords::parse({"a　b\n"}, stop_limits), "stopwords.entry");
        failure(StopWords::parse({std::string("a\0b", 3)}, stop_limits), "stopwords.nul");
        failure(StopWords::parse({"abc", std::string_view("\xff", 1)}, {100, 1, 1, 1}),
                "stopwords.utf8");
        failure(analyze_tokens({"a", "b", std::string("\xff", 1)}, empty.value(),
                               {100, 100, 100, 1, 1, 1, 1}),
                "frequency.utf8");
        failure(analyze_tokens({std::string("a\0b", 3)}, empty.value(), token_limits),
                "frequency.nul");
        for (const std::string &word : std::vector<std::string>{"", "a b", "a\tb", "a　b"}) {
            failure(analyze_tokens({word}, empty.value(), token_limits), "frequency.token");
        }
        failure(analyze_tokens({"the"}, stops.value(), {1, 3, 2, 1, 3, 1, 3}),
                "frequency.token_bytes");
        // 与规范化同一Unicode15.1 White_Space集合；边缘可trim，token内部不可保留。
        const std::vector<std::string> whites{"\t",           "\n",           "\v",
                                              "\f",           "\r",           " ",
                                              "\xc2\x85",     "\xc2\xa0",     "\xe1\x9a\x80",
                                              "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82",
                                              "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85",
                                              "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88",
                                              "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
                                              "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f",
                                              "\xe3\x80\x80"};
        for (const auto &white : whites) {
            auto trimmed = StopWords::parse({white + "A" + white}, stop_limits);
            check(trimmed && trimmed.value().contains("a"),
                  "all specified whitespace trims consistently");
            failure(analyze_tokens({"a" + white + "b"}, empty.value(), token_limits),
                    "frequency.token");
        }
        // 公开TF值也必须验，不相信调用方手填的total/count；极限计数无需制造海量token。
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        const RecommendationLimits wide{maximum, 100, 100, 1000, maximum};
        failure(accumulate_recommendation({}, {{{"a", 0}}, 0}, wide), "recommendation.table");
        failure(accumulate_recommendation({}, {{{"a", 1}}, 2}, wide), "recommendation.table");
        failure(accumulate_recommendation({{{"a", 1}}, 1, 0}, {}, wide), "recommendation.table");
        for (const std::string &word : std::vector<std::string>{"", "a b", "a　b"}) {
            failure(accumulate_recommendation({}, {{{word, 1}}, 1}, wide), "recommendation.term");
        }
        failure(accumulate_recommendation({}, {{{std::string("\xff", 1), 1}}, 1}, wide),
                "recommendation.utf8");
        failure(accumulate_recommendation({}, {{{std::string("a\0", 2), 1}}, 1}, wide),
                "recommendation.nul");
        const RecommendationFrequencies big{{{"a", maximum}}, maximum, 1};
        auto exactly = accumulate_recommendation({}, {{{"a", maximum}}, maximum}, wide);
        check(exactly && exactly.value().total_tokens == maximum,
              "UINT64_MAX exact representable frequency");
        failure(accumulate_recommendation(big, {{{"a", 1}}, 1}, wide), "recommendation.overflow");
        check(big.terms.at("a") == maximum && big.total_tokens == maximum && big.documents == 1,
              "overflow leaves input intact");
        failure(accumulate_recommendation({{}, 0, maximum}, {}, wide), "recommendation.overflow");
        failure(accumulate_recommendation({}, {{{"a", maximum}, {"b", 1}}, 0}, wide),
                "recommendation.overflow");
        auto limited = recommendation_limits;
        limited.max_distinct_terms = 2;
        const RecommendationFrequencies before{{{"a", 1}}, 1, 1};
        failure(accumulate_recommendation(before, {{{"b", 1}, {"c", 1}}, 2}, limited),
                "recommendation.distinct_terms");
        check(before.terms == FrequencyTable{{"a", 1}} && before.total_tokens == 1 &&
                  before.documents == 1,
              "late merge failure leaves current unchanged");
        auto successful = accumulate_recommendation(before, doc, recommendation_limits);
        check(successful && successful.value().terms == FrequencyTable{{"a", 2}, {"中国", 2}},
              "overlapping terms sum and new keys merge");
    } catch (const std::exception &e) {
        ++failures;
        std::cerr << "unexpected: " << e.what() << '\n';
    }
    std::cout << "term frequency checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
