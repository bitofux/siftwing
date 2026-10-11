/*
 * PROJECT : SIFTWING
 * FILE    : duplicate_decision.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 按来源规范次序、已保留者与比较预算生成完整去重报告
 * IMPLEMENTATION : 先完整校验身份，再排序借用指针；仅保留有指纹候选作为后续匹配依据
 */
#include "siftwing/document/duplicate_decision.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace siftwing::document {
namespace {
using Result = base::Result<DuplicateReport>;

// 与读取层相对来源标识规则一致；此处只验证定位值，不操作文件系统或猜测编码。
bool valid_source(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..") { return false; }
        if (end == std::string_view::npos) { return true; }
        begin = end + 1;
    }
    return false;
}

Result failure(const char* message, const char* context) {
    return Result::failure({message, context});
}

bool source_less(const DuplicateCandidate* lhs, const DuplicateCandidate* rhs) {
    if (lhs->source_id.input_path != rhs->source_id.input_path) {
        return lhs->source_id.input_path < rhs->source_id.input_path;
    }
    return lhs->source_id.record_ordinal < rhs->source_id.record_ordinal;
}
} // namespace

base::Result<DuplicateReport>
decide_duplicates(const std::vector<DuplicateCandidate>& candidates,
                  DuplicatePolicy policy, const DuplicateLimits& limits) {
    if (policy.max_hamming_distance > 64) {
        return failure("threshold must be in 0..64", "duplicate.policy");
    }
    if (limits.max_documents == 0 || limits.max_source_path_bytes == 0 ||
        limits.max_total_source_path_bytes == 0 || limits.max_comparisons == 0) {
        return failure("all duplicate budgets must be positive", "duplicate.limits");
    }
    const auto count = base::checked_narrow<std::uint64_t>(candidates.size(), "duplicate.overflow");
    if (!count) { return Result::failure(count.error()); }
    if (count.value() > limits.max_documents) {
        return failure("candidate count exceeds budget", "duplicate.documents");
    }

    std::uint64_t source_bytes = 0;
    std::set<base::DocumentId> ids;
    std::vector<const DuplicateCandidate*> ordered;
    ordered.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const auto size = base::checked_narrow<std::uint64_t>(candidate.source_id.input_path.size(),
                                                             "duplicate.overflow");
        if (!size) { return Result::failure(size.error()); }
        if (size.value() > limits.max_source_path_bytes) {
            return failure("source path exceeds budget", "duplicate.source_path_bytes");
        }
        const auto total = base::checked_add(source_bytes, size.value(), "duplicate.overflow");
        if (!total) { return Result::failure(total.error()); }
        source_bytes = total.value();
        if (source_bytes > limits.max_total_source_path_bytes) {
            return failure("total source bytes exceed budget", "duplicate.total_source_path_bytes");
        }
        if (!valid_source(candidate.source_id.input_path)) {
            return failure("invalid relative source identifier", "duplicate.source");
        }
        if (!ids.insert(candidate.doc_id).second) {
            return failure("duplicate document identity", "duplicate.doc_id");
        }
        ordered.push_back(&candidate);
    }
    std::sort(ordered.begin(), ordered.end(), source_less);
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        const auto& a = ordered[i - 1]->source_id;
        const auto& b = ordered[i]->source_id;
        if (a.input_path == b.input_path && a.record_ordinal == b.record_ordinal) {
            return failure("duplicate source position", "duplicate.source");
        }
    }

    DuplicateReport report{policy, {}, {}};
    report.counts.input = count.value();
    report.decisions.reserve(candidates.size());
    std::vector<const DuplicateCandidate*> retained;
    retained.reserve(candidates.size());
    for (const auto* candidate : ordered) {
        DuplicateDecision decision{candidate->doc_id, candidate->source_id,
                                    DuplicateDisposition::no_features, std::nullopt};
        if (!candidate->fingerprint) {
            ++report.counts.no_features;
        } else {
            for (const auto* earlier : retained) {
                // 在递增前检查上限，最大uint64预算也不会回绕；不提前估算n*(n-1)/2。
                if (report.counts.comparisons == limits.max_comparisons) {
                    return failure("fingerprint comparison budget exhausted", "duplicate.comparisons");
                }
                ++report.counts.comparisons;
                const auto distance = hamming_distance(*candidate->fingerprint, *earlier->fingerprint);
                if (distance <= policy.max_hamming_distance) {
                    decision.match = DuplicateMatch{earlier->doc_id, distance};
                    break; // 按接纳次序取第一命中，不改成最近者或传递聚类。
                }
            }
            if (decision.match) {
                decision.disposition = DuplicateDisposition::duplicate;
                ++report.counts.duplicates;
            } else {
                decision.disposition = DuplicateDisposition::kept;
                retained.push_back(candidate);
                ++report.counts.kept;
            }
        }
        // 每项只进入一种计数；计数总和不超过已受检的候选数，无额外计数溢出。
        report.decisions.push_back(std::move(decision));
    }
    return Result::success(std::move(report));
}
} // namespace siftwing::document
