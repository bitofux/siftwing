/*
 * PROJECT : SIFTWING
 * FILE    : simhash_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 受限接收原创TF批次，输出指纹数值供独立任意精度参考验证
 */
#include "siftwing/document/simhash.h"

#include <iostream>
#include <limits>
#include <sstream>

int main() {
    try {
        std::string bytes;
        char byte = 0;
        while (std::cin.get(byte)) {
            if (bytes.size() == 8U * 1024U * 1024U) { return 2; }
            bytes.push_back(byte);
        }
        if (!std::cin.eof()) { return 1; }
        std::istringstream stream(bytes);
        unsigned documents = 0;
        if (!(stream >> documents) || documents > 512) { return 2; }
        for (unsigned document = 0; document < documents; ++document) {
            unsigned entries = 0;
            siftwing::text::DocumentTermFrequencies tf;
            if (!(stream >> tf.total_tokens >> entries) || entries > 4096) { return 2; }
            for (unsigned entry = 0; entry < entries; ++entry) {
                std::string hex, word;
                std::uint64_t weight = 0;
                if (!(stream >> hex >> weight) || hex.size() % 2 != 0) { return 2; }
                for (std::size_t i = 0; i < hex.size(); i += 2) {
                    const auto digit = [](char value) -> int {
                        if (value >= '0' && value <= '9') { return value - '0'; }
                        if (value >= 'a' && value <= 'f') { return value - 'a' + 10; }
                        return -1;
                    };
                    const auto high = digit(hex[i]), low = digit(hex[i + 1]);
                    if (high < 0 || low < 0) { return 2; }
                    word.push_back(static_cast<char>((high * 16) + low));
                }
                if (!tf.terms.emplace(std::move(word), weight).second) { return 2; }
            }
            constexpr auto max = std::numeric_limits<std::uint64_t>::max();
            auto result = siftwing::document::make_simhash(tf, {4096, 1048576, 8388608, max});
            if (!result) {
                std::cerr << result.error().context << '\n';
                return 1;
            }
            if (result.value()) { std::cout << result.value()->bits << '\n'; }
            else { std::cout << "null\n"; }
        }
        stream >> std::ws;
        if (!stream.eof()) { return 2; }
        return std::cout ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
