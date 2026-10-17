/*
 * PROJECT : SIFTWING
 * FILE    : summary_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 用十六进制原字节协议暴露摘要文本及元数据，供独立Unicode参考核验
 */
#include "../support/summary_queries.hpp"
#include "siftwing/query/summary.hpp"
#include <iostream>

namespace {
std::string decode(const std::string &hex) {
    if (hex == "-") {
        return {};
    }
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("odd hex");
    }
    std::string out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto value = std::stoul(hex.substr(i, 2), nullptr, 16);
        out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
    }
    return out;
}
std::string encode(std::string_view bytes) {
    if (bytes.empty()) {
        return "-";
    }
    const char *digits = "0123456789abcdef";
    std::string out;
    for (char value : bytes) {
        const auto byte = static_cast<unsigned char>(value);
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 15U]);
    }
    return out;
}
} // namespace
int main() {
    try {
        siftwing::query::SummaryOptions options;
        siftwing::query::SummaryLimits limits;
        std::string words, body;
        // 仅测试协议：十六进制允许空正文/NUL/坏UTF-8，不形成正式CLI或读取私有输入。
        while (std::cin >> words >> body >> options.window_scalars
               >> options.leading_context_scalars >> limits.max_content_bytes
               >> limits.max_query_terms >> limits.max_query_bytes >> limits.max_match_pairs
               >> limits.max_window_scalars) {
            const auto q = siftwing::test::summary_query(decode(words));
            const auto content = decode(body);
            const auto r = siftwing::query::make_summary(q, content, options, limits);
            if (!r) {
                std::cout << "ERR " << r.error().context << '\n';
                continue;
            }
            const auto &s = r.value();
            std::cout << "OK " << encode(s.text) << ' ' << s.source_byte_offset << ' ' << s.matched
                      << ' ' << s.truncated_before << ' ' << s.truncated_after << '\n';
        }
        return std::cin.eof() ? 0 : 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
