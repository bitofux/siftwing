/*
 * PROJECT : SIFTWING
 * FILE    : html_fragment_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 从标准输入接收有界片段并调用HTML抽取，用hex输出本地验收结果
 * TEST CONTRACT :
 * -- 仅验收探针，无HTML文件发现或RSS生产适配；超输入预算前不增长缓冲区
 * -- 五项预算由调用方显式提供，JSON仅为测试格式，正文全部归报告拥有
 */

#include "siftwing/document/html_text.h"

#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {
std::uint64_t positive(std::string_view input) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || value == 0) {
        throw std::invalid_argument("positive uint64 limits required");
    }
    return value;
}

void hex(std::string_view input) {
    constexpr char digits[] = "0123456789abcdef";
    std::cout.put('"');
    for (const char value : input) {
        const auto byte = static_cast<unsigned char>(value);
        std::cout.put(digits[byte >> 4U]);
        std::cout.put(digits[byte & 15U]);
    }
    std::cout.put('"');
}

int run(int argc, char** argv) {
    if (argc != 6) { throw std::invalid_argument("usage: probe INPUT_BYTES OUTPUT_BYTES TOKENS NODES DEPTH < fragment"); }
    const siftwing::document::HtmlTextLimits limits{positive(argv[1]), positive(argv[2]), positive(argv[3]),
                                                  positive(argv[4]), positive(argv[5])};
    std::string input;
    std::array<char, 4096> buffer{};
    while (std::cin) {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = std::cin.gcount();
        if (count > 0) {
            const auto size = static_cast<std::uint64_t>(count);
            if (input.size() > limits.max_input_bytes || size > limits.max_input_bytes - input.size()) {
                std::cout << "{\"error_context\":\"html.input_bytes\"}\n";
                return 1;
            }
            input.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (!std::cin.eof()) { throw std::runtime_error("input stream failed"); }
    auto result = siftwing::document::extract_html_text(input, limits);
    if (!result) {
        std::cout << "{\"error_context_hex\":"; hex(result.error().context); std::cout << "}\n";
        return 1;
    }
    std::cout << "{\"text_hex\":"; hex(result.value().text);
    std::cout << ",\"recovered\":" << (result.value().recovered ? "true" : "false") << "}\n";
    std::cout.flush();
    return std::cout ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
