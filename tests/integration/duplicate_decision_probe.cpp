/*
 * PROJECT : SIFTWING
 * FILE    : duplicate_decision_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 为独立Python参考暴露批量纯决策，不复制生产比较或排序算法
 */
#include "siftwing/document/duplicate_decision.h"

#include <iostream>
#include <limits>
#include <stdexcept>

int main() {
    try {
        using namespace siftwing::document;
        std::size_t cases = 0;
        if (!(std::cin >> cases)) { return 2; }
        for (std::size_t c = 0; c < cases; ++c) {
            std::size_t count = 0;
            std::uint32_t threshold = 0;
            std::uint64_t budget = 0;
            if (!(std::cin >> count >> threshold >> budget)) { return 2; }
            std::vector<DuplicateCandidate> input;
            for (std::size_t i = 0; i < count; ++i) {
                std::uint64_t identity = 0, ordinal = 0, bits = 0;
                unsigned present = 0;
                std::string path;
                if (!(std::cin >> identity >> path >> ordinal >> present >> bits)) { return 2; }
                input.push_back({siftwing::base::DocumentId::from_integer(identity).value(),
                                 {path,ordinal}, present ? std::optional<SimHash64>{SimHash64{bits}} :
                                                         std::nullopt});
            }
            const auto max = std::numeric_limits<std::uint64_t>::max();
            const auto r = decide_duplicates(input, {threshold}, {max,max,max,budget});
            if (!r) { std::cout << "error " << r.error().context << '\n'; continue; }
            const auto& report = r.value();
            std::cout << "ok " << report.counts.input << ' ' << report.counts.kept << ' '
                      << report.counts.duplicates << ' ' << report.counts.no_features << ' '
                      << report.counts.comparisons << '\n';
            for (const auto& d : report.decisions) {
                const char* status = d.disposition == DuplicateDisposition::kept ? "kept" :
                    d.disposition == DuplicateDisposition::duplicate ? "duplicate" : "no_features";
                std::cout << d.doc_id.value() << ' ' << d.source_id.input_path << ' '
                          << d.source_id.record_ordinal << ' ' << status << ' ';
                if (d.match) { std::cout << d.match->retained_id.value() << ' ' << d.match->distance; }
                else { std::cout << "- -"; }
                std::cout << '\n';
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
    return 0;
}
