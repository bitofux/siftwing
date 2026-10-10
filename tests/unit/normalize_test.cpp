/*
 * PROJECT : SIFTWING
 * FILE    : normalize_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 以原创输入和独立手写预期验证规范化规则、容量、编码与拥有性
 */

#include "siftwing/text/normalize.h"

#include <array>
#include <clocale>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace siftwing::text;
unsigned checks = 0;
unsigned failures = 0;
const NormalizationLimits generous{1024 * 1024, 1024 * 1024};
void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
void expected(std::string_view input, std::string_view wanted, const char* label, NormalizationLimits limits = generous) {
    const auto result = normalize_utf8(input, limits);
    check(result && result.value() == wanted, label);
    if (result) {
        const auto repeated = normalize_utf8(result.value(), generous);
        check(repeated && repeated.value() == result.value(), "normalization is idempotent with sufficient budget");
    }
}
void error(std::string_view input, NormalizationLimits limits, std::string_view context) {
    const auto result = normalize_utf8(input, limits);
    check(!result && result.error().context == context, "expected failure has the exact context and no string payload");
}
void rules() {
    check(normalization_policy_version == 1, "fixed policy version identifies these rules");
    expected({}, "", "default empty view is a valid empty input");
    expected(" \t\r\n\v\f　\u0085\u00a0", "", "all whitespace succeeds empty");
    expected("\r\nHELLO\t WORLD\rTail\n", "hello world tail", "ASCII case and line endings become common text");
    expected("  中文 ABC　2026.10 +1 -0.5 C++ foo_bar isn't e-mail!  ", "中文 abc 2026.10 +1 -0.5 c++ foo_bar isn't e-mail!", "numbers and punctuation remain and do not join neighboring terms");
    expected("É Σ İ Straße ＡＢ１２\u0301", "É Σ İ straße ＡＢ１２\u0301", "nonASCII case width and combining marks remain unchanged");
    expected("A\u200bB\u2060C\ufeffD\u180eE\u001cF", "a\u200bb\u2060c\ufeffd\u180ee\u001cf", "format characters and nonWhite_Space controls are preserved");
    expected("\ufeff \ufeffABC", "\ufeff \ufeffabc", "BOM scalar is preserved even at start and remains idempotent");
    expected("😀\U0010ffff𠀀", "😀\U0010ffff𠀀", "four-byte scalars including legal noncharacters survive");
    expected("e\u0301 é", "e\u0301 é", "canonical equivalents are not silently merged without NFC");
    // 独立列出属性全部25码点，不以被测分类函数生成预期；每一项夹在实字符之间。
    const std::array<std::string_view, 25> spaces{{"\t","\n","\v","\f","\r"," ","\u0085","\u00a0","\u1680",
        "\u2000","\u2001","\u2002","\u2003","\u2004","\u2005","\u2006","\u2007","\u2008","\u2009","\u200a",
        "\u2028","\u2029","\u202f","\u205f","\u3000"}};
    for (const auto ws : spaces) {
        expected(std::string(ws) + "A" + std::string(ws) + std::string(ws) + "B" + std::string(ws), "a b", "each Unicode White_Space trims and collapses");
    }
    for (char c = 'A'; c <= 'Z'; ++c) {
        const char lower = static_cast<char>(c + ('a' - 'A'));
        expected(std::string(1, c), std::string(1, lower), "each ASCII capital folds deterministically");
    }
    for (char c = '!'; c <= '~'; ++c) {
        if (c >= 'A' && c <= 'Z') { continue; }
        expected(std::string(1, c), std::string(1, c), "all other printable ASCII remains including every punctuation and digit");
    }
}
void encoding_and_limits() {
    for (const auto& invalid : std::vector<std::string>{"\xff", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80", "\xf8\x88\x80\x80\x80", "\xe2\x28\xa1"}) {
        error(invalid, generous, "normalize.utf8");
        const auto result = normalize_utf8("ABC" + invalid, generous);
        check(!result && result.error().message.find("byte 3") != std::string::npos, "invalid tail reports sequence start byte rather than valid prefix");
    }
    const std::string with_nul{"AB\0CD", 5};
    error(with_nul, generous, "normalize.nul");
    const auto nul = normalize_utf8(with_nul, generous);
    check(!nul && nul.error().message.find("byte 2") != std::string::npos, "NUL position is a byte offset");
    error({}, {0, 1}, "normalize.limits"); error({}, {1, 0}, "normalize.limits");
    expected("A", "a", "exact input and output bytes are inclusive", {1, 1});
    expected("A　　　　", "a", "discarded trailing whitespace never causes interim output overlimit", {13, 1});
    expected("　　　　", "", "whitespace-only input can exceed output limit and still be empty", {12, 1});
    expected("中", "中", "limits count UTF8 bytes rather than scalars", {3, 3});
    error("中", {3, 2}, "normalize.output_bytes"); error("中", {2, 3}, "normalize.input_bytes");
    error("A B", {3, 2}, "normalize.output_bytes"); expected("A B", "a b", "delayed separator participates in final budget", {3, 3});
    error("long\xff", {100, 1}, "normalize.utf8"); error(with_nul, {100, 1}, "normalize.nul");
    error("\xff", {1, 0}, "normalize.limits"); error("AA\xff", {2, 100}, "normalize.input_bytes");
    expected("ABC", "abc", "max representable limits do not cause arithmetic overflow", {std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()});
}
void ownership_and_repetition() {
    std::string owned;
    {
        std::string input = "  A　中文\r\nB ";
        auto result = normalize_utf8(input, generous);
        owned = std::move(result).value(); input.assign(input.size(), 'x');
    }
    check(owned == "a 中文 b", "returned string owns bytes after input result and limits scope ends");
    auto owned_error = [] { std::string input = "AB\xff"; return normalize_utf8(input, generous); }();
    check(!owned_error && owned_error.error().message.find("byte 2") != std::string::npos, "failure diagnostic owns its storage after input destruction");
    for (unsigned i = 0; i < 30; ++i) { expected("TXT\tRSS  2026", "txt rss 2026", "repeated calls have independent state"); }
    const char* previous = std::setlocale(LC_CTYPE, nullptr);
    const std::string saved = previous ? previous : "C";
    for (const char* locale : {"C", "C.UTF-8"}) {
        if (std::setlocale(LC_CTYPE, locale)) { expected("I É 中文\u00a0A", "i É 中文 a", "available locales cannot change classification"); }
    }
    check(std::setlocale(LC_CTYPE, saved.c_str()) != nullptr, "test restores original locale");
}
} // namespace
int main() {
    try { rules(); encoding_and_limits(); ownership_and_repetition(); }
    catch (const std::exception& e) { ++failures; std::cerr << "FAIL: unexpected exception: " << e.what() << '\n'; }
    std::cout << "Normalization checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
