/*
 * PROJECT : SIFTWING
 * FILE    : highlight_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 以原字节十六进制协议暴露HTML与区间，供独立参考校验
 */
#include "../support/summary_queries.hpp"
#include "siftwing/query/highlight.hpp"
#include <iostream>
#include <sstream>

namespace {
unsigned nibble(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned>(c - 'A' + 10);
    }
    throw std::runtime_error("invalid hex");
}
std::string decode(std::string_view hex) {
    if (hex == "-") {
        return {};
    }
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("odd hex");
    }
    std::string out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<char>(
            static_cast<unsigned char>(nibble(hex[i]) * 16 + nibble(hex[i + 1]))));
    }
    return out;
}
std::string encode(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    if (bytes.empty()) {
        return "-";
    }
    std::string out;
    for (char c : bytes) {
        const auto value = static_cast<unsigned char>(c);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 15]);
    }
    return out;
}
} // namespace
int main() {
    try {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.size() > 1048576) {
                return 2;
            }
            std::istringstream row{line};
            std::string words, body, extra;
            siftwing::query::HighlightLimits limits;
            if (!(row >> words >> body >> limits.max_text_bytes >> limits.max_query_terms
                    >> limits.max_query_bytes >> limits.max_match_pairs >> limits.max_spans
                    >> limits.max_output_bytes)
                || row >> extra) {
                return 2;
            }
            const auto q = siftwing::test::summary_query(decode(words));
            const auto out = siftwing::query::make_highlight(q, decode(body), limits);
            if (!out) {
                std::cout << "ERR " << out.error().context << '\n';
                continue;
            }
            std::cout << "OK " << encode(out.value().html) << ' ' << out.value().spans.size();
            for (const auto &span : out.value().spans) {
                std::cout << ' ' << span.byte_offset << ':' << span.byte_length;
            }
            std::cout << '\n';
        }
        return std::cin.eof() && std::cout ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
