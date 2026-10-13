/*
 * PROJECT : SIFTWING
 * FILE    : frame_codec_mutation_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 固定种子损坏与任意字节性质回归，Release仍执行全部检查
 */
#include "frame_codec_properties.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::uint64_t state = 0x5346545700020005ULL;
// 明确uint64模运算及位移的PRNG使输入不依赖标准库distribution实现，可逐次重放。
std::uint64_t next() {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}
std::string wire(std::string body) {
    const unsigned char header[] = {
        0x53, 0x46, 0x54, 0x57, 1, 24, 0, 3, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 3, 4, 5, 6, 7, 8};
    std::string out(reinterpret_cast<const char *>(header), sizeof header);
    auto length = body.size();
    for (std::size_t i = 16; i > 12; --i) {
        out[i - 1] = static_cast<char>(length % 256);
        length /= 256;
    }
    return out + body;
}
} // namespace
int main() {
    using siftwing::testing::exercise_codec;
    std::vector<std::string> seeds{"",
        "{}",
        "[]",
        "{\"x\":\"中文\"}",
        std::string("{}\0x", 4),
        "{\"x\":1e999}",
        "{\"x\":\"\\uD800\"}",
        wire("{}"),
        wire("{]"),
        wire("{\"x\":\"中文\"}")};
    std::string deep = "{\"x\":";
    deep.append(64, '[');
    deep += '0';
    deep.append(64, ']');
    deep += '}';
    seeds.push_back(deep);
    const auto valid = wire("{}");
    seeds.push_back(valid + valid + valid.substr(0, 23));
    std::size_t inputs = 0;
    for (const auto &seed : seeds) {
        exercise_codec(seed);
        ++inputs;
    }
    // 全部黄金头/Body的单字节替换与所有末尾前缀，补随机样本不一定命中的控制字段。
    for (std::size_t i = 0; i < valid.size(); ++i) {
        exercise_codec(std::string_view(valid).substr(0, i));
        ++inputs;
        for (unsigned int byte = 0; byte < 256; ++byte) {
            auto mutated = valid;
            mutated[i] = static_cast<char>(byte);
            exercise_codec(mutated);
            ++inputs;
        }
    }
    for (std::size_t i = 0; i < 20000; ++i) {
        auto bytes = seeds[static_cast<std::size_t>(next() % seeds.size())];
        for (unsigned int n = 0; n < 1U + static_cast<unsigned int>(next() % 8); ++n) {
            const auto action = next() % 4;
            const auto at = static_cast<std::size_t>(next() % (bytes.size() + 1));
            if (action == 0 && at < bytes.size())
                bytes[at] ^= static_cast<char>(1U << (next() % 8));
            else if (action == 1 && at < bytes.size())
                bytes.erase(at, 1);
            else if (action == 2 && bytes.size() < 4096)
                bytes.insert(at, 1, static_cast<char>(next() % 256));
            else if (at < bytes.size())
                bytes[at] = static_cast<char>(next() % 256);
        }
        exercise_codec(bytes);
        ++inputs;
    }
    for (std::size_t i = 0; i < 4000; ++i) {
        std::string random(static_cast<std::size_t>(next() % 513), '\0');
        for (auto &byte : random)
            byte = static_cast<char>(next() % 256);
        exercise_codec(random);
        ++inputs;
    }
    // 比运行max_len大的有界输入也重放，65537字节由入口直接忽略。
    exercise_codec(wire(std::string(65500, ' ') + "{}"));
    ++inputs;
    exercise_codec(std::string(65537, 'x'));
    ++inputs;
    std::cout << "seed=0x5346545700020005 inputs=" << inputs << " failures=0\n";
    return 0;
}
