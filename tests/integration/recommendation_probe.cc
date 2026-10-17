/*
 * PROJECT : SIFTWING
 * FILE    : recommendation_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 独立参考逐行hex词键/预算探针，协议仅用于测试而非业务持久化或CLI
 */
#include "siftwing/query/recommendation.hpp"
#include "siftwing/base/checked.hpp"
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
unsigned digit(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    throw std::invalid_argument("bad hex");
}
std::string unhex(std::string_view value) {
    if (value == "-") {
        return {};
    }
    if (value.size() % 2) {
        throw std::invalid_argument("odd hex");
    }
    std::string out;
    for (std::size_t i = 0; i < value.size(); i += 2) {
        out.push_back(static_cast<char>(digit(value[i]) * 16U + digit(value[i + 1])));
    }
    return out;
}
std::string hex(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char byte : value) {
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 15U]);
    }
    return out;
}
std::uint64_t number(const std::string &raw) {
    std::size_t used = 0;
    if (raw.empty() || raw.front() == '-') {
        throw std::invalid_argument("unsigned required");
    }
    const auto n = std::stoull(raw, &used);
    if (used != raw.size()) {
        throw std::invalid_argument("bad number");
    }
    return n;
}
void run(const std::string &line) {
    using namespace siftwing;
    std::istringstream input(line);
    std::string raw, key_hex;
    if (!(input >> key_hex >> raw)) {
        throw std::invalid_argument("missing input");
    }
    const auto k = number(raw);
    std::uint64_t budgets[8]{};
    for (auto &budget : budgets) {
        if (!(input >> raw)) {
            throw std::invalid_argument("missing budget");
        }
        budget = number(raw);
    }
    text::RecommendationFrequencies f;
    f.documents = 1;
    while (input >> raw) {
        const auto colon = raw.find(':');
        if (colon == raw.npos) {
            throw std::invalid_argument("missing frequency");
        }
        const auto frequency = number(raw.substr(colon + 1));
        if (!f.terms.emplace(unhex(std::string_view{raw}.substr(0, colon)), frequency).second) {
            throw std::invalid_argument("duplicate fixture key");
        }
        const auto total = base::checked_add(f.total_tokens, frequency);
        if (!total) {
            throw std::invalid_argument("fixture total overflow");
        }
        f.total_tokens = total.value();
    }
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    const auto dictionary =
        index::RecommendationDictionary::build(f, {{max, max, max, max, max}, max, max, max, max})
            .value();
    const query::RecommendationLimits limits{budgets[0],
        budgets[1],
        budgets[2],
        budgets[3],
        budgets[4],
        budgets[5],
        budgets[6],
        budgets[7]};
    const auto result = query::recommend_terms(unhex(key_hex), dictionary, k, limits);
    if (!result) {
        std::cout << "{\"error\":\"" << result.error().context << "\"}\n";
        return;
    }
    std::cout << "{\"terms\":[";
    bool first = true;
    for (const auto &item : result.value()) {
        if (!first) {
            std::cout << ',';
        }
        first = false;
        std::cout << "[\"" << hex(item.term) << "\"," << item.term_id.value() << ','
                  << item.distance << ',' << item.frequency << ']';
    }
    std::cout << "]}\n";
}
} // namespace
int main() {
    try {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.size() > 2000000) {
                return 2;
            }
            run(line);
        }
        return std::cin.eof() && std::cout ? 0 : 3;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 3;
    }
}
