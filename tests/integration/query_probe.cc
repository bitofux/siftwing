/*
 * PROJECT : SIFTWING
 * FILE    : query_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 原创独立参考的查询探针，流式接收hex字节与显式选项
 */
#include "siftwing/query/query.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <charconv>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
std::uint64_t number(const std::string &text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || text.empty()) {
        throw std::runtime_error("bad test integer");
    }
    return value;
}
std::string decode(const std::string &hex) {
    if (hex == "_") {
        return {};
    }
    if (hex.size() % 2) {
        throw std::runtime_error("bad test hex");
    }
    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a' + 10);
        }
        throw std::runtime_error("bad test hex");
    };
    std::string out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<char>((nibble(hex[i]) << 4U) | nibble(hex[i + 1])));
    }
    return out;
}
std::string encode(const std::string &bytes) {
    const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char byte : bytes) {
        out += digits[byte >> 4U];
        out += digits[byte & 15U];
    }
    return out;
}
} // namespace
int main() {
    try {
        using namespace siftwing;
        const auto pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
            text::StopWords::parse({"the\n"}, {100, 10, 10, 100}).value())
                                  .value();
        for (std::string line; std::getline(std::cin, line);) {
            std::istringstream fields{line};
            std::string mode, k, max, hex, extra;
            if (!(fields >> mode >> k >> max >> hex) || fields >> extra) {
                return 2;
            }
            const auto selection = number(mode);
            const auto selected = selection == 0   ? query::QueryMode::all
                                  : selection == 1 ? query::QueryMode::any
                                                   : static_cast<query::QueryMode>(99);
            const query::QueryLimits limits{
                {{1000, 1000}, {1000, 100, 100, 1000}, {100, 1000, 100, 100, 1000, 100, 1000}},
                number(max)};
            const auto result =
                query::Query::from_text(decode(hex), *pipeline, {selected, number(k)}, limits);
            if (!result) {
                std::cout << "ERROR " << result.error().context << '\n';
                continue;
            }
            const auto &value = result.value();
            std::cout << "OK " << (value.mode() == query::QueryMode::all ? 0 : 1) << ' '
                      << value.top_k() << ' ' << value.frequencies().total_tokens << ' '
                      << value.frequencies().terms.size() << '\n';
            for (const auto &[word, count] : value.frequencies().terms) {
                std::cout << "T " << encode(word) << ' ' << count << '\n';
            }
        }
        return std::cin.bad() || !std::cout ? 3 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
