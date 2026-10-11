/*
 * PROJECT : SIFTWING
 * FILE    : simhash.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 校验完整TF后以无符号票数计算稳定指纹
 * IMPLEMENTATION :
 * -- 字节哈希固定unsigned值和模2^64运算，避免char符号与标准库哈希变化
 * -- 每位只累加正票，以total-positive计算负票，支持UINT64_MAX权重且无signed溢出
 * -- 全输入验证先于投票，空特征明确返回nullopt，不发布部分计算结果
 */
#include "siftwing/document/simhash.h"
#include "siftwing/base/checked.h"

#include <utf8.h>

#include <array>
#include <utility>

namespace siftwing::document {
namespace {
using FingerprintResult = base::Result<std::optional<SimHash64>>;
FingerprintResult fail(std::string context, std::string message) {
    return FingerprintResult::failure({std::move(message), std::move(context)});
}
bool has_space(std::string_view word) {
    auto cursor = word.begin();
    while (cursor != word.end()) {
        const auto cp = utf8::next(cursor, word.end());
        if ((cp >= 9U && cp <= 13U) || cp == 0x20U || cp == 0x85U || cp == 0xa0U ||
            cp == 0x1680U || (cp >= 0x2000U && cp <= 0x200aU) || cp == 0x2028U ||
            cp == 0x2029U || cp == 0x202fU || cp == 0x205fU || cp == 0x3000U) {
            return true;
        }
    }
    return false;
}
// FNV-1a按UTF8原字节消费；fmix64扩散各位，减少相近ASCII词仅在少数位变化的偏差。
// 常量属于规则版本1，不调用平台相关std::hash；非加密，不保证无碰撞或语言学相似。
std::uint64_t word_hash(std::string_view word) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char byte : word) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 33U;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33U;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33U;
    return hash;
}
} // namespace

base::Result<std::optional<SimHash64>>
make_simhash(const text::DocumentTermFrequencies &frequencies, const SimHashLimits &limits) {
    if (limits.max_distinct_terms == 0 || limits.max_term_bytes == 0 ||
        limits.max_vocabulary_bytes == 0 || limits.max_total_tokens == 0) {
        return fail("simhash.limits", "four positive fingerprint limits required");
    }
    if (frequencies.terms.size() > limits.max_distinct_terms) {
        return fail("simhash.distinct_terms", "unique term count exceeds budget");
    }
    std::uint64_t bytes = 0;
    for (const auto &entry : frequencies.terms) {
        if (entry.first.size() > limits.max_term_bytes) {
            return fail("simhash.term_bytes", "term exceeds byte budget");
        }
        if (entry.first.size() > limits.max_vocabulary_bytes - bytes) {
            return fail("simhash.vocabulary_bytes", "unique term bytes exceed budget");
        }
        bytes += entry.first.size();
    }
    std::size_t index = 0;
    for (const auto &entry : frequencies.terms) {
        const auto invalid = utf8::find_invalid(entry.first.begin(), entry.first.end());
        if (invalid != entry.first.end()) {
            return fail("simhash.utf8", "term " + std::to_string(index) + " invalid UTF-8 at byte " +
                                           std::to_string(invalid - entry.first.begin()));
        }
        const auto nul = entry.first.find('\0');
        if (nul != entry.first.npos) {
            return fail("simhash.nul", "term " + std::to_string(index) + " NUL at byte " +
                                          std::to_string(nul));
        }
        ++index;
    }
    std::uint64_t total = 0;
    for (const auto &entry : frequencies.terms) {
        if (entry.first.empty() || has_space(entry.first)) {
            return fail("simhash.term", "term must be nonempty without whitespace");
        }
        if (entry.second == 0) {
            return fail("simhash.table", "term frequencies must be positive");
        }
        auto next = base::checked_add(total, entry.second, "simhash.overflow");
        if (!next) {
            return FingerprintResult::failure(std::move(next).error());
        }
        total = next.value();
    }
    if (total != frequencies.total_tokens) {
        return fail("simhash.table", "term frequencies do not match total_tokens");
    }
    if (total > limits.max_total_tokens) {
        return fail("simhash.total_tokens", "total term frequencies exceed budget");
    }
    if (frequencies.terms.empty()) {
        return FingerprintResult::success(std::nullopt);
    }
    std::array<std::uint64_t, 64> positive{};
    for (const auto &entry : frequencies.terms) {
        const auto hash = word_hash(entry.first);
        for (std::size_t bit = 0; bit < positive.size(); ++bit) {
            if ((hash & (std::uint64_t{1} << bit)) != 0) {
                // 此位正票始终为已校验total的子集，累加不可能超过UINT64_MAX。
                positive[bit] += entry.second;
            }
        }
    }
    SimHash64 output;
    for (std::size_t bit = 0; bit < positive.size(); ++bit) {
        if (positive[bit] > total - positive[bit]) {
            output.bits |= std::uint64_t{1} << bit;
        }
    }
    return FingerprintResult::success(output);
}

std::uint32_t hamming_distance(SimHash64 lhs, SimHash64 rhs) noexcept {
    auto different = lhs.bits ^ rhs.bits;
    std::uint32_t count = 0;
    // 每次清除最低的1；最多64轮，无C++20 popcount或编译器builtin依赖。
    while (different != 0) {
        different &= different - 1U;
        ++count;
    }
    return count;
}
} // namespace siftwing::document
