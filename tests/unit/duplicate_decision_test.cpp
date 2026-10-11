/*
 * PROJECT : SIFTWING
 * FILE    : duplicate_decision_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 手算链式/多命中、全部阈值边界和输入排列证明确定性去重合同
 */
#include "siftwing/document/duplicate_decision.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing::document;
using siftwing::base::DocumentId;
unsigned checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
DocumentId id(std::uint64_t value) { return DocumentId::from_integer(value).value(); }
DuplicateCandidate candidate(std::uint64_t value, std::string path, std::uint64_t ordinal,
                             std::optional<SimHash64> fingerprint) {
    return {id(value), {std::move(path), ordinal}, fingerprint};
}
const DuplicateLimits broad{100, 100, 10000, 10000};
constexpr auto max = std::numeric_limits<std::uint64_t>::max();
void invariant(const DuplicateReport& r) {
    check(r.counts.input == r.decisions.size(), "all input has a decision");
    check(r.counts.input == r.counts.kept + r.counts.duplicates + r.counts.no_features,
          "counts partition input");
    std::vector<DocumentId> retained;
    for (const auto& d : r.decisions) {
        check(d.match.has_value() == (d.disposition == DuplicateDisposition::duplicate),
              "match exists only for duplicate");
        if (d.match) {
            check(std::find(retained.begin(), retained.end(), d.match->retained_id) != retained.end(),
                  "match points only to an earlier retained identity");
            check(d.match->distance <= r.policy.max_hamming_distance, "reported match is within policy");
        }
        if (d.disposition == DuplicateDisposition::kept) { retained.push_back(d.doc_id); }
    }
}
void error(const std::vector<DuplicateCandidate>& input, DuplicatePolicy policy,
           const DuplicateLimits& limits, const char* context) {
    const auto r = decide_duplicates(input, policy, limits);
    check(!r, "invalid input or exhausted budget fails");
    if (!r) {
        check(r.error().context == context && !r.error().message.empty(), "precise failure diagnosis");
        bool absent = false;
        try { (void)r.value(); } catch (const std::bad_variant_access&) { absent = true; }
        check(absent, "failure has no partial decisions or counts");
    }
}
} // namespace

int main() {
    try {
        check(DuplicatePolicy{}.max_hamming_distance == 3, "default threshold is three");
        const auto empty = decide_duplicates({}, {}, broad).value();
        invariant(empty);
        check(empty.counts.comparisons == 0 && empty.policy.max_hamming_distance == 3,
              "empty batch succeeds with zero counts and actual policy");
        // A=000000、B=000111、C=111111：AB和BC为3，AC为6，丢弃B后C必须保留。
        const std::vector<DuplicateCandidate> chain{
            candidate(80, "a", 0, SimHash64{0}), candidate(2, "b", 0, SimHash64{7}),
            candidate(0, "c", 0, SimHash64{63})};
        const auto chained = decide_duplicates(chain, {}, broad).value();
        invariant(chained);
        check(chained.counts.kept == 2 && chained.counts.duplicates == 1 &&
              chained.counts.comparisons == 2, "only retained comparisons, no transitive closure");
        check(chained.decisions[1].match && chained.decisions[1].match->retained_id == id(80) &&
              chained.decisions[1].match->distance == 3 &&
              chained.decisions[2].disposition == DuplicateDisposition::kept,
              "chain fixes inclusive threshold and zero identity legality");
        // A=0、B=15距离4均保留；C=7先距A3，再距B1，应取最早A而不是最近B。
        auto multi = chain;
        multi[1].fingerprint = SimHash64{15}; multi[2].fingerprint = SimHash64{7};
        const auto matched = decide_duplicates(multi, {}, broad).value();
        invariant(matched);
        check(matched.decisions[2].match && matched.decisions[2].match->retained_id == id(80) &&
              matched.decisions[2].match->distance == 3 && matched.counts.comparisons == 2,
              "first retained hit beats nearest fingerprint and smallest identity");
        // 全部65种距离与65种阈值，用低位掩码手算预期，不从生产距离函数获得答案。
        for (std::uint32_t threshold = 0; threshold <= 64; ++threshold) {
            for (std::uint32_t distance = 0; distance <= 64; ++distance) {
                const auto mask = distance == 64 ? max : (std::uint64_t{1} << distance) - 1;
                auto pair = std::vector<DuplicateCandidate>{candidate(0,"a",0,SimHash64{0}),
                                                          candidate(max,"b",max,SimHash64{mask})};
                const auto r = decide_duplicates(pair, {threshold}, broad).value();
                const bool duplicate = distance <= threshold;
                check(r.decisions[1].disposition == (duplicate ? DuplicateDisposition::duplicate :
                                                               DuplicateDisposition::kept),
                      "all threshold-distance boundary pairs");
                check(r.counts.kept == (duplicate ? 1U : 2U) && r.counts.comparisons == 1,
                      "range-wide counts and exact comparison count");
                check(!duplicate || (r.decisions[1].match && r.decisions[1].match->distance == distance),
                      "range-wide audited distance");
            }
        }
        const std::vector<DuplicateCandidate> canonical{
            candidate(80,"a",9,SimHash64{0}), candidate(20,"b",2,SimHash64{15}),
            candidate(90,"b",7,SimHash64{7}), candidate(0,"c",0,std::nullopt),
            candidate(40,"c",max,std::nullopt), candidate(max,"d",0,SimHash64{max})};
        // 六候选的720排列得到同一审计结果；同文件按序号、不按ID、也不按调用输入次序。
        std::array<unsigned,6> order{0,1,2,3,4,5};
        do {
            std::vector<DuplicateCandidate> shuffled;
            for (auto i : order) { shuffled.push_back(canonical[i]); }
            const auto r = decide_duplicates(shuffled, {}, broad).value();
            check(r.counts.kept == 3 && r.counts.duplicates == 1 && r.counts.no_features == 2 &&
                  r.counts.comparisons == 4, "all input permutations preserve counts");
            for (std::size_t i = 0; i < canonical.size(); ++i) {
                check(r.decisions[i].doc_id == canonical[i].doc_id &&
                      r.decisions[i].source_id.input_path == canonical[i].source_id.input_path &&
                      r.decisions[i].source_id.record_ordinal == canonical[i].source_id.record_ordinal,
                      "canonical source order for every permutation");
            }
            check(r.decisions[2].match && r.decisions[2].match->retained_id == id(80),
                  "permutations preserve earliest match");
        } while (std::next_permutation(order.begin(), order.end()));
        auto nulls = std::vector<DuplicateCandidate>{candidate(1,"a",0,std::nullopt),
                                                   candidate(2,"b",0,std::nullopt)};
        const auto no_features = decide_duplicates(nulls, {64}, {2,1,2,1}).value();
        invariant(no_features);
        check(no_features.counts.no_features == 2 && no_features.counts.kept == 0 &&
              no_features.counts.duplicates == 0 && no_features.counts.comparisons == 0,
              "absence never creates a duplicate even at threshold64");
        const auto owned = [&] {
            auto temporary = chain;
            return decide_duplicates(temporary, {}, broad).value();
        }();
        check(owned.decisions[1].source_id.input_path == "b" &&
              owned.decisions[1].match->retained_id == id(80), "result survives input destruction");
        auto mutable_input = chain;
        const auto result = decide_duplicates(mutable_input, {}, broad).value();
        mutable_input[0].source_id.input_path = "changed";
        mutable_input[0].fingerprint = std::nullopt;
        check(result.decisions[0].source_id.input_path == "a", "source result owns independent storage");
        error(chain, {65}, {}, "duplicate.policy");
        for (unsigned which = 0; which < 4; ++which) {
            auto limits = broad;
            if (which == 0) { limits.max_documents = 0; }
            if (which == 1) { limits.max_source_path_bytes = 0; }
            if (which == 2) { limits.max_total_source_path_bytes = 0; }
            if (which == 3) { limits.max_comparisons = 0; }
            error({}, {}, limits, "duplicate.limits");
        }
        error(chain, {}, {2,1,3,2}, "duplicate.documents");
        error({candidate(1,"aa",0,std::nullopt)}, {}, {1,1,2,1}, "duplicate.source_path_bytes");
        error(chain, {}, {3,1,2,2}, "duplicate.total_source_path_bytes");
        error(chain, {}, {3,1,3,1}, "duplicate.comparisons");
        const auto exact = decide_duplicates(chain, {}, {3,1,3,2}).value();
        check(exact.counts.comparisons == 2, "all exact budgets accepted");
        // 不估算最坏两两比较；实际首命中短路只需2次，预算2应成功。
        auto repeated = chain; repeated[2].fingerprint = SimHash64{7};
        check(decide_duplicates(repeated, {}, {3,1,3,2}).value().counts.comparisons == 2,
              "comparison budget measures actual work rather than quadratic bound");
        for (const std::string path : {"", "/a", ".", "..", "a/", "a//b", "a/./b", "a/../b"}) {
            error({candidate(1,path,0,std::nullopt)}, {}, broad, "duplicate.source");
        }
        error({candidate(1,std::string("a\0b",3),0,std::nullopt)}, {}, broad, "duplicate.source");
        auto duplicate_ids = nulls; duplicate_ids[1].doc_id = id(1);
        error(duplicate_ids, {}, broad, "duplicate.doc_id");
        auto duplicate_sources = nulls; duplicate_sources[1].source_id = duplicate_sources[0].source_id;
        error(duplicate_sources, {}, broad, "duplicate.source");
        // 来源层沿读取合同的原字节std::string排序，不新增编码/路径规范化规则。
        check(static_cast<bool>(decide_duplicates({candidate(1,std::string("x\xff",2),0,SimHash64{0})},
                                                   {}, broad)), "source identifiers are literal bytes");
        check(static_cast<bool>(decide_duplicates(chain, {}, {max,max,max,max})), "full uint64 budgets legal");
        check(chain[0].source_id.input_path == "a" && chain[0].fingerprint->bits == 0 &&
              chain[1].doc_id == id(2), "success and failures leave caller inputs unchanged");
    } catch (const std::exception& e) {
        ++failures; std::cerr << "unexpected: " << e.what() << '\n';
    }
    std::cout << "duplicate decision checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
