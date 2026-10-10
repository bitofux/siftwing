/*
 * PROJECT : SIFTWING
 * FILE    : cppjieba_tokenizer_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 受限stdin到拥有中文/混合token的hex报告，仅供独立参考验收
 */

#include "siftwing/text/cppjieba_tokenizer.h"

#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {
std::uint64_t positive(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
        throw std::invalid_argument("positive limits required");
    }
    return value;
}
void hex(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        std::cout.put(digits[byte >> 4U]);
        std::cout.put(digits[byte & 15U]);
    }
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 10 || (std::string_view(argv[9]) != "0" && std::string_view(argv[9]) != "1")) {
        return 2;
    }
    try {
        const siftwing::text::TokenizationLimits limits{positive(argv[1]), positive(argv[2]),
                                                        positive(argv[3]), positive(argv[4])};
        std::string input;
        std::array<char, 4096> buffer{};
        // 装载预算由工具承担；生产接口只接受调用方已装载字节，不能回收该成本。
        while (std::cin) {
            std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > limits.max_input_bytes - input.size()) {
                std::cout << "{\"error_context\":\"probe.input_bytes\"}\n";
                return 1;
            }
            input.append(buffer.data(), count);
        }
        if (std::cin.bad()) {
            return 1;
        }
        auto created = siftwing::text::CppJiebaTokenizer::create(
            {argv[5], argv[6],
             std::string(argv[7]) == "-" ? std::nullopt : std::optional<std::string>{argv[7]},
             positive(argv[8]), std::string(argv[9]) == "1"});
        if (!created) {
            std::cout << "{\"error_context\":\"" << created.error().context << "\"}\n";
            return 1;
        }
        const siftwing::text::Tokenizer &tokenizer = *created.value();
        auto result = tokenizer.tokenize(input, limits);
        if (!result) {
            std::cout << "{\"error_context\":\"" << result.error().context
                      << "\",\"message_hex\":\"";
            hex(result.error().message);
            std::cout << "\"}\n";
            return 1;
        }
        std::cout << "{\"policy_version\":" << siftwing::text::cppjieba_tokenization_policy_version
                  << ",\"tokens_hex\":[";
        bool first = true;
        for (const auto &token : result.value()) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << '"';
            hex(token);
            std::cout << '"';
        }
        std::cout << "]}\n";
        return std::cout ? 0 : 1;
    } catch (const std::invalid_argument &) {
        return 2;
    } catch (const std::exception &) {
        return 1;
    }
}
