/*
 * PROJECT : SIFTWING
 * FILE    : normalize_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 受限stdin到规范化hex报告的验收工具，不提供生产文件加载或管线
 */

#include "siftwing/text/normalize.h"

#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {
std::uint64_t positive(std::string_view text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0) { throw std::invalid_argument("positive byte limits required"); }
    return value;
}
void hex(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        std::cout.put(digits[byte >> 4U]); std::cout.put(digits[byte & 15U]);
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 3) { return 2; }
    try {
        const siftwing::text::NormalizationLimits limits{positive(argv[1]), positive(argv[2])};
        std::string input;
        std::array<char, 4096> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > limits.max_input_bytes - input.size()) { std::cout << "{\"error_context\":\"probe.input_bytes\"}\n"; return 1; }
            input.append(buffer.data(), count);
        }
        if (!std::cin.eof()) { return 1; }
        auto result = siftwing::text::normalize_utf8(input, limits);
        if (!result) {
            std::cout << "{\"error_context\":\"" << result.error().context << "\",\"message_hex\":\"";
            hex(result.error().message); std::cout << "\"}\n"; return 1;
        }
        std::cout << "{\"policy_version\":" << siftwing::text::normalization_policy_version << ",\"text_hex\":\"";
        hex(result.value()); std::cout << "\"}\n"; return std::cout ? 0 : 1;
    } catch (const std::invalid_argument&) { return 2; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
