/*
 * PROJECT : SIFTWING
 * FILE    : english_tokenizer.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 完整编码校验后，预计算预算并复制拥有型英文token
 * IMPLEMENTATION :
 * -- 完整UTF-8校验优先，ASCII分类不猜测无效高位字节的含义
 * -- 两遍共用最大连续段扫描，计数通过后才分配输出
 * -- 数量和累计字节先减法比较再增长，失败不泄露已扫描前缀
 */

#include "siftwing/text/english_tokenizer.h"
#include "siftwing/base/checked.h"

#include <utf8.h>

#include <utility>

namespace siftwing::text {
namespace {

bool word_byte(unsigned char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9');
}

// input已完整验证；UTF-8非ASCII序列的每一字节都>=128，不会误入ASCII词段。
// consume同步借用词段而不保存view；末尾词段同样交付，不依赖输入含终止分隔符。
template <typename Consume>
bool visit_words(std::string_view input, Consume consume) {
    std::size_t position = 0;
    while (position < input.size()) {
        if (!word_byte(static_cast<unsigned char>(input[position]))) { ++position; continue; }
        const auto begin = position;
        do { ++position; }
        while (position < input.size() && word_byte(static_cast<unsigned char>(input[position])));
        if (!consume(input.substr(begin, position - begin))) { return false; }
    }
    return true;
}

base::Result<TokenSequence> failure(std::string message, std::string context) {
    return base::Result<TokenSequence>::failure({std::move(message), std::move(context)});
}

} // namespace

base::Result<TokenSequence> EnglishTokenizer::tokenize(
    std::string_view input, const TokenizationLimits& limits) const {
    if (limits.max_input_bytes == 0 || limits.max_tokens == 0 ||
        limits.max_token_bytes == 0 || limits.max_output_bytes == 0) {
        return failure("tokenization requires four positive limits", "tokenizer.limits");
    }
    const auto input_size = base::checked_narrow<std::uint64_t>(input.size(), "tokenizer.input_bytes");
    if (!input_size || input_size.value() > limits.max_input_bytes) {
        return failure("tokenization input exceeds byte limit", "tokenizer.input_bytes");
    }
    const auto invalid = utf8::find_invalid(input.begin(), input.end());
    if (invalid != input.end()) {
        return failure("invalid UTF-8 sequence at byte " + std::to_string(invalid - input.begin()), "tokenizer.utf8");
    }
    const auto nul = input.find('\0');
    if (nul != input.npos) { return failure("NUL at byte " + std::to_string(nul), "tokenizer.nul"); }

    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    const char* failed_context = nullptr;
    const bool fits = visit_words(input, [&](std::string_view word) {
        // 词段不长于已通过uint64输入预算的input；两个累计值始终不大于各自预算。
        const auto size = static_cast<std::uint64_t>(word.size());
        if (size > limits.max_token_bytes) { failed_context = "tokenizer.token_bytes"; return false; }
        if (count == limits.max_tokens) { failed_context = "tokenizer.tokens"; return false; }
        if (size > limits.max_output_bytes - bytes) { failed_context = "tokenizer.output_bytes"; return false; }
        ++count;
        bytes += size;
        return true;
    });
    if (!fits) { return failure("token sequence exceeds limit", failed_context); }
    const auto reserve_count = base::checked_narrow<std::size_t>(count, "tokenizer.tokens");
    if (!reserve_count) { return failure("token count cannot be represented", "tokenizer.tokens"); }
    TokenSequence output;
    output.reserve(reserve_count.value());
    static_cast<void>(visit_words(input, [&](std::string_view word) {
        output.emplace_back(word);
        return true;
    }));
    return base::Result<TokenSequence>::success(std::move(output));
}

} // namespace siftwing::text
