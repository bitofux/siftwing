/*
 * PROJECT : SIFTWING
 * FILE    : top_k_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 验证条数边界、精确同分前缀、原诊断与完整工作准入
 */
#include "siftwing/query/top_k.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <cfenv>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool yes, const char *message) {
    ++checks;
    if (!yes) {
        throw std::runtime_error(message);
    }
}
base::DocumentId id(std::uint64_t value) {
    return base::DocumentId::from_integer(value).value();
}
index::WeightedIndex build(const std::vector<index::IndexDocument> &documents) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto raw = index::InvertedIndex::build(
        documents, {{1000, 10000, 100, 1000, 10000}, 100, maximum, maximum, 100});
    if (!raw) {
        throw std::runtime_error(raw.error().context);
    }
    return index::WeightedIndex::build(std::move(raw).value(), {100, 1000, 10000}).value();
}
void error(const base::Result<query::RankedDocuments> &r, const char *context) {
    check(!r && r.error().context == context, context);
}
struct RoundingGuard {
    int original = std::fegetround();
    ~RoundingGuard() {
        (void)std::fesetround(original);
    }
};
} // namespace
int main() {
    try {
        RoundingGuard guard;
        check(std::fesetround(FE_TONEAREST) == 0, "nearest");
        using namespace query;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({}, {100, 10, 10, 100}).value())
                            .value();
        const QueryLimits ql{{{10000, 10000},
                                 {10000, 1000, 1000, 10000},
                                 {1000, 10000, 1000, 1000, 10000, 1000, 10000}},
            maximum};
        const auto q =
            [&](std::string_view word, std::uint64_t k = 1, QueryMode mode = QueryMode::any) {
                return Query::from_text(word, *pipeline, {mode, k}, ql).value();
            };
        const TopKLimits limits{{{100, 1000, 100}, 10000}, maximum};
        auto index = build({{id(maximum), {{{"a", 3}, {"b", 4}}, 7}},
            {id(0), {{{"a", 3}, {"b", 4}}, 7}},
            {id(7), {{{"a", 1}}, 1}}});
        const auto before = index.serialize({100000}).value();
        const auto query = q("a", 1);
        const auto full = rank_candidates(query, index, limits.ranking).value();
        // 独立手算：doc7只有a，余弦1；两相同3:4向量精确同分，ID0先于MAX。
        check(full.size() == 3 && full[0].document_id == id(7) && full[0].score == 1,
            "handwritten best identity");
        check(full[1].document_id == id(0) && full[2].document_id == id(maximum)
                  && full[1].score == full[2].score,
            "exact tie endpoints");
        for (auto k :
            {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3}, std::uint64_t{4}, maximum}) {
            const auto result = rank_top_k(q("a", k), index, limits).value();
            check(result.size() == (k < 3 ? static_cast<std::size_t>(k) : 3), "min(k,C) boundary");
            for (std::size_t i = 0; i < result.size(); ++i) {
                check(result[i].document_id == full[i].document_id
                          && result[i].score == full[i].score,
                    "exact original score and prefix");
            }
        }
        check(rank_top_k(q("a", 2), index, {limits.ranking, 2}).value().size() == 2,
            "k budget endpoint");
        error(rank_top_k(q("a", 3), index, {limits.ranking, 2}), "top_k.top_k");
        error(rank_top_k(query, index, {}), "top_k.limits");
        error(rank_top_k(q("a", 3), index, {{}, 2}), "top_k.top_k");
        error(rank_top_k(query, index, {{}, 1}), "ranking.limits");
        // 完整预算拒绝必须在截断之前发生；同时比较原错误的全部文本。
        for (const auto &budget : std::vector<RankingLimits>{{{1, 1000, 100}, 10000},
                 {{100, 2, 100}, 10000},
                 {{100, 1000, 2}, 10000},
                 {{100, 1000, 100}, 2}}) {
            const auto input = q("a b", 1);
            const auto reference = rank_candidates(input, index, budget);
            const auto result = rank_top_k(input, index, {budget, maximum});
            check(!reference && !result && reference.error().context == result.error().context
                      && reference.error().message == result.error().message,
                "full diagnostic preserved before k");
        }
        for (auto mode : {QueryMode::all, QueryMode::any}) {
            check(rank_top_k(q("unknown", maximum, mode), index, limits).value().empty(),
                "all unknown empty");
            check(rank_top_k(q("a", 1, mode), build({}), limits).value().empty(), "empty index");
        }
        check(rank_top_k(q("a unknown", 1, QueryMode::all), index, limits).value().empty(),
            "AND unknown");
        const auto partial = rank_top_k(q("a unknown unknown"), index, limits).value();
        check(partial[0].document_id == id(7) && partial[0].score == 1, "OR unknown ignored");
        const auto repeated = rank_top_k(q("a a b b b b b"), index, limits).value();
        check(repeated[0].document_id == id(0), "query repetition changes relevance");
        for (auto mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
            if (std::fesetround(mode) != 0) {
                continue;
            }
            error(rank_top_k(query, index, limits), "ranking.rounding");
            error(rank_top_k(query, index, {limits.ranking, 0}), "top_k.limits");
            error(rank_top_k(q("a", 2), index, {limits.ranking, 1}), "top_k.top_k");
            check(std::fegetround() == mode, "never changes rounding");
        }
        check(std::fesetround(FE_TONEAREST) == 0, "restore nearest");
        auto moved = q("a");
        const auto destination = std::move(moved);
        error(rank_top_k(moved, index, limits), "candidate.query");
        check(!rank_top_k(destination, index, limits).value().empty(), "moved query destination");
        auto output = rank_top_k(query, index, limits).value();
        auto copy = output;
        copy.clear();
        check(output.size() == 1 && copy.empty(), "independent result copy");
        check(index.serialize({100000}).value() == before && query.frequencies().terms.at("a") == 1,
            "all success and failures preserve inputs");
        const auto moved_index = std::move(index);
        check(rank_top_k(query, index, limits).value().empty(), "moved index source empty");
        output = rank_top_k(query, moved_index, limits).value();
        pipeline.reset();
        check(rank_top_k(query, moved_index, limits).value()[0].score == output[0].score,
            "query survives pipeline");
        std::cout << "top-k unit checks=" << checks << " failures=0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
