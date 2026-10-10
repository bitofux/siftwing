/*
 * PROJECT : SIFTWING
 * FILE    : text_pipeline_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用显式hex文字批次验收管线，测试IO与业务内存入口分离
 */
#include "../support/cppjieba_resources.h"
#include "siftwing/text/english_tokenizer.h"
#include "siftwing/text/text_pipeline.h"

#include <iostream>

namespace {
using namespace siftwing::text;
std::string read(const char *path) {
    if (std::string_view(path) == "-") {
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("stopword source open failed");
    }
    std::string bytes;
    char c = 0;
    while (stream.get(c)) {
        if (bytes.size() == 65536) {
            throw std::runtime_error("stopword source too large");
        }
        bytes.push_back(c);
    }
    if (!stream.eof()) {
        throw std::runtime_error("stopword source read failed");
    }
    return bytes;
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
void hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        std::cout.put(digits[b >> 4U]);
        std::cout.put(digits[b & 15U]);
    }
}
int error(const siftwing::base::Error &diagnostic) {
    std::cout << "{\"error_context\":\"" << diagnostic.context << "\"}\n";
    return 1;
}
} // namespace

int main(int argc, char **argv) {
    if (!(argc == 2 && std::string_view(argv[1]) == "--fixture") &&
        !(argc == 6 && std::string_view(argv[1]) == "--files")) {
        return 2;
    }
    try {
        std::unique_ptr<CppJiebaTokenizer> chinese;
        std::string first, second;
        if (argc == 2) {
            siftwing::test::Resources resources;
            auto created = CppJiebaTokenizer::create(resources.config());
            if (!created) {
                return error(created.error());
            }
            chinese = std::move(created).value();
            first = "\xef\xbb\xbf THE\r\nAND\n中文\n";
        } else {
            auto created = CppJiebaTokenizer::create({argv[2], argv[3], std::nullopt, 16777216, true});
            if (!created) {
                return error(created.error());
            }
            chinese = std::move(created).value();
            first = read(argv[4]);
            second = read(argv[5]);
        }
        auto stops = StopWords::parse({first, second}, {131072, 10000, 4096, 131072});
        if (!stops) {
            return error(stops.error());
        }
        auto en = TextPipeline::create(std::make_unique<EnglishTokenizer>(), stops.value());
        auto cn = TextPipeline::create(std::move(chinese), std::move(stops).value());
        if (!en || !cn) {
            return 1;
        }
        const TextPipelineLimits limits{{16777216, 16777216}, {16777216, 3000000, 32768, 16777216},
                                          {3000000, 16777216, 32768, 3000000, 16777216, 1000000, 16777216}};
        std::vector<TokenAnalysis> documents;
        std::string line;
        std::size_t input_bytes = 0;
        while (std::getline(std::cin, line)) {
            if (line.empty() || (line[0] != 'E' && line[0] != 'C')) {
                return 2;
            }
            if (documents.size() == 100 || line.size() > 33554433 ||
                line.size() > 134217728 - input_bytes) {
                return 1;
            }
            input_bytes += line.size();
            const auto text = unhex(std::string_view(line).substr(1));
            auto analyzed = (line[0] == 'E' ? en.value() : cn.value())->analyze(text, limits);
            if (!analyzed) {
                return error(analyzed.error());
            }
            documents.push_back(std::move(analyzed).value());
        }
        if (!std::cin.eof()) {
            return 1;
        }
        // 单份JSON报告容纳完整批次，不提供JSONL导入或交换接口。
        std::cout << "{\"documents\":[";
        bool first_document = true;
        for (const auto &document : documents) {
            if (!first_document) {
                std::cout << ',';
            }
            first_document = false;
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
            std::cout << "],\"total_tokens\":" << document.tf.total_tokens << ",\"terms\":[";
            bool first_term = true;
            for (const auto &entry : document.tf.terms) {
                if (!first_term) {
                    std::cout << ',';
                }
                first_term = false;
                std::cout << "[\"";
                hex(entry.first);
                std::cout << "\"," << entry.second << ']';
            }
            std::cout << "]}";
        }
        std::cout << "]}\n";
        return std::cout ? 0 : 1;
    } catch (const std::invalid_argument &) {
        return 2;
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
