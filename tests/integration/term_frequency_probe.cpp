/*
 * PROJECT : SIFTWING
 * FILE    : term_frequency_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 原创/私有验收的显式停用词字节与hex-token文档报告，无生产装载合同
 */
#include "siftwing/text/term_frequency.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
using namespace siftwing::text;
std::string read(const char *name) {
    if (std::string_view(name) == "-") {
        return {};
    }
    std::ifstream stream(name, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("source open failed");
    }
    std::string bytes;
    char value = 0;
    while (stream.get(value)) {
        if (bytes.size() == 65536) {
            throw std::runtime_error("source too large");
        }
        bytes.push_back(value);
    }
    if (!stream.eof()) {
        throw std::runtime_error("source read failed");
    }
    return bytes;
}
void hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    for (const char value : bytes) {
        const auto b = static_cast<unsigned char>(value);
        std::cout.put(digits[b >> 4U]);
        std::cout.put(digits[b & 15U]);
    }
}
unsigned digit(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    throw std::invalid_argument("bad hex");
}
std::string unhex(std::string_view line) {
    if (line.size() % 2 != 0) {
        throw std::invalid_argument("odd hex");
    }
    std::string bytes;
    bytes.reserve(line.size() / 2);
    for (std::size_t i = 0; i < line.size(); i += 2) {
        bytes.push_back(static_cast<char>(digit(line[i]) * 16U + digit(line[i + 1])));
    }
    return bytes;
}
void table(const FrequencyTable &terms) {
    std::cout << '[';
    bool first = true;
    for (const auto &entry : terms) {
        if (!first) {
            std::cout << ',';
        }
        first = false;
        std::cout << "[\"";
        hex(entry.first);
        std::cout << "\"," << entry.second << ']';
    }
    std::cout << ']';
}
int error(const siftwing::base::Error &diagnostic) {
    std::cout << "{\"error_context\":\"" << diagnostic.context << "\"}\n";
    return 1;
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 3) {
        return 2;
    }
    try {
        const auto a = read(argv[1]), b = read(argv[2]);
        auto stopwords = StopWords::parse({a, b}, {131072, 10000, 4096, 131072});
        if (!stopwords) {
            return error(stopwords.error());
        }
        std::vector<TokenSequence> documents;
        std::string line;
        std::size_t count = 0, bytes = 0;
        while (std::getline(std::cin, line)) {
            if (line.size() > 65536) {
                return 1;
            }
            if (line == "@") {
                if (documents.size() == 1000) {
                    return 1;
                }
                documents.emplace_back();
                continue;
            }
            if (documents.empty()) {
                return 2;
            }
            if (++count > 3000000 || line.size() > 67108864 - bytes) {
                return 1;
            }
            bytes += line.size();
            documents.back().push_back(unhex(line));
        }
        if (!std::cin.eof()) {
            return 1;
        }
        RecommendationFrequencies cumulative;
        std::vector<TokenAnalysis> analyzed;
        for (const auto &document : documents) {
            auto current =
                analyze_tokens(document, stopwords.value(),
                               {3000000, 33554432, 32768, 3000000, 33554432, 1000000, 33554432});
            if (!current) {
                return error(current.error());
            }
            auto next = accumulate_recommendation(cumulative, current.value().tf,
                                                  {1000, 1000000, 32768, 33554432, 3000000});
            if (!next) {
                return error(next.error());
            }
            cumulative = std::move(next).value();
            analyzed.push_back(std::move(current).value());
        }
        std::cout << "{\"stopwords_hex\":[";
        bool first = true;
        for (const auto &entry : stopwords.value().entries()) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << '"';
            hex(entry);
            std::cout << '"';
        }
        std::cout << "],\"documents\":[";
        first = true;
        for (const auto &document : analyzed) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << "{\"tokens_hex\":[";
            bool first_token = true;
            for (const auto &token : document.tokens) {
                if (!first_token) {
                    std::cout << ',';
                }
                first_token = false;
                std::cout << '"';
                hex(token);
                std::cout << '"';
            }
            std::cout << "],\"total_tokens\":" << document.tf.total_tokens << ",\"terms\":";
            table(document.tf.terms);
            std::cout << '}';
        }
        std::cout << "],\"recommendation\":{\"documents\":" << cumulative.documents
                  << ",\"total_tokens\":" << cumulative.total_tokens << ",\"terms\":";
        table(cumulative.terms);
        std::cout << "}}\n";
        return std::cout ? 0 : 1;
    } catch (const std::invalid_argument &) {
        return 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
