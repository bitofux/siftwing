/*
 * PROJECT : SIFTWING
 * FILE    : english_tokenizer_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用手写预期验证英文词边界、完整失败、拥有性及多态合同
 */

#include "siftwing/text/english_tokenizer.h"

#include <array>
#include <clocale>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
using siftwing::text::EnglishTokenizer;
using siftwing::text::TokenizationLimits;
using siftwing::text::TokenSequence;
unsigned checks = 0;
unsigned failures = 0;
constexpr TokenizationLimits generous{65536, 65536, 65536, 65536};

void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
void expected(std::string_view input, const TokenSequence& words, const char* label,
              TokenizationLimits limits = generous) {
    const EnglishTokenizer concrete;
    const siftwing::text::Tokenizer& tokenizer = concrete;
    const auto result = tokenizer.tokenize(input, limits);
    check(result && result.value() == words, label);
}
void error(std::string_view input, TokenizationLimits limits, std::string_view context) {
    const auto result = EnglishTokenizer{}.tokenize(input, limits);
    check(!result && result.error().context == context, "failure context selects the contracted cause");
    bool rejects_value = false;
    try { static_cast<void>(result.value()); }
    catch (const std::bad_variant_access&) { rejects_value = true; }
    check(rejects_value, "failure does not expose a partial token sequence");
}

void rules() {
    expected({}, {}, "empty input succeeds empty including default empty view");
    expected(" ,;!\t\n_中文😀é\ufeff", {}, "only separators including nonASCII succeed empty");
    expected("One ONE one 123 a1 B2 123", {"One","ONE","one","123","a1","B2","123"},
             "case order digits and duplicate tokens are preserved without hidden normalization");
    expected("don't e-mail foo_bar C++ 3.14 -12.5 a/b\\c @d#e", {"don","t","e","mail","foo","bar","C","3","14","12","5","a","b","c","d","e"},
             "punctuation splits rather than joins or introduces language-specific rules");
    expected("café résumé 中文A𠀀B😀C a\u0301b Ａ１２ Z", {"caf","r","sum","A","B","C","a","b","Z"},
             "all nonASCII scalars delimit without transliteration Unicode case folding or Chinese tokenization");
    expected("\ufeffA\ufeffB\u200bC\u2060D\u180eE\u001cF", {"A","B","C","D","E","F"},
             "BOM zero-width and controls are delimiters at every position");
    expected("word", {"word"}, "EOF emits a final token without trailing delimiter");
    expected("---word---", {"word"}, "leading repeated and trailing delimiters do not emit empties");
    // 分类参考来自独立枚举的ASCII字符集合，不调用生产分类器；NUL属于失败用例。
    constexpr std::string_view members = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (unsigned value = 1; value < 128; ++value) {
        const char c = static_cast<char>(value);
        const bool word = members.find(c) != members.npos;
        expected(std::string(1,c), word ? TokenSequence{std::string(1,c)} : TokenSequence{},
                 "all nonNUL ASCII singleton classes match the independent explicit set");
        expected(std::string("a") + c + "b", word ? TokenSequence{std::string("a") + c + "b"} : TokenSequence{"a","b"},
                 "every ASCII byte either joins a maximal run or separates both neighbors");
    }
    for (const auto c : std::array<std::string_view, 9>{"\u0085","\u00a0","　","é","Σ","中","😀","\U0010ffff","\u0301"}) {
        expected(std::string("A") + std::string(c) + "B", {"A","B"}, "valid multibyte scalars never merge ASCII neighbors");
    }
}

void encoding_and_limits() {
    for (const auto& invalid : std::vector<std::string>{"\xff","\x80","\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82","\xf8\x88\x80\x80\x80","\xe2\x28\xa1"}) {
        error(invalid, generous, "tokenizer.utf8");
        const auto result = EnglishTokenizer{}.tokenize("中AB" + invalid, generous);
        check(!result && result.error().message.find("byte 5") != std::string::npos,
              "invalid tail is located by byte offset rather than scalar count");
    }
    const std::string nul{"AB\0CD",5};
    error(nul, generous, "tokenizer.nul");
    const auto result = EnglishTokenizer{}.tokenize(nul, generous);
    check(!result && result.error().message.find("byte 2") != std::string::npos, "NUL position is owned diagnostic text");
    for (const auto limits : std::array<TokenizationLimits,4>{{{0,1,1,1},{1,0,1,1},{1,1,0,1},{1,1,1,0}}}) {
        error({}, limits, "tokenizer.limits");
    }
    expected("Ab 12", {"Ab","12"}, "all four exact budgets are inclusive", {5,2,2,4});
    error("Ab 12", {4,2,2,4}, "tokenizer.input_bytes");
    error("Ab 12", {5,1,2,4}, "tokenizer.tokens");
    error("Ab 12", {5,2,1,4}, "tokenizer.token_bytes");
    error("Ab 12", {5,2,2,3}, "tokenizer.output_bytes");
    expected("中文", {}, "input budget counts UTF8 bytes but nonASCII consumes no token budget", {6,1,1,1});
    error("中文", {5,1,1,1}, "tokenizer.input_bytes");
    expected("A,　,", {"A"}, "delimiters consume input bytes but not total output bytes", {6,1,1,1});
    expected("x", {"x"}, "single token consumes one count and one output byte", {1,1,1,1});
    error("x xx", {4,1,1,1}, "tokenizer.token_bytes");
    error("x x", {3,1,1,1}, "tokenizer.tokens");
    error("x x", {3,2,1,1}, "tokenizer.output_bytes");
    error("aaaaa\xff", {6,1,1,1}, "tokenizer.utf8");
    error(std::string("aaaa\0",5), {5,1,1,1}, "tokenizer.nul");
    error(std::string("\0\xff",2), generous, "tokenizer.utf8");
    error("\xff", {0,1,1,1}, "tokenizer.limits");
    error("x\xff", {1,1,1,1}, "tokenizer.input_bytes");
    const auto max = std::numeric_limits<std::uint64_t>::max();
    expected("A1 B2 A1", {"A1","B2","A1"}, "max limits do not overflow counters", {max,max,max,max});
    expected(std::string(60000,'a'), {std::string(60000,'a')}, "long EOF token remains complete", {60000,1,60000,60000});
    error(std::string(60000,'a'), {60000,1,59999,60000}, "tokenizer.token_bytes");
}

void ownership_and_polymorphism() {
    TokenSequence words;
    {
        std::string input = "A1, B2 A1";
        const EnglishTokenizer tokenizer;
        auto result = tokenizer.tokenize(input, generous);
        check(result && input == "A1, B2 A1", "call does not overwrite input");
        words = std::move(result).value();
        input.assign(input.size(), 'x');
    }
    check(words == TokenSequence{"A1","B2","A1"}, "tokens outlive overwritten input result and tokenizer");
    auto failure = [] { std::string input = "中\xff"; return EnglishTokenizer{}.tokenize(input, generous); }();
    check(!failure && failure.error().message.find("byte 3") != std::string::npos, "error strings own their bytes after inputs disappear");
    // 独立派生实现证明接口可扩展及通过基类拥有指针销毁，避免只测直接英文调用。
    struct OtherTokenizer final : siftwing::text::Tokenizer {
        bool& destroyed;
        explicit OtherTokenizer(bool& flag) : destroyed(flag) {}
        ~OtherTokenizer() noexcept override { destroyed = true; }
        siftwing::base::Result<TokenSequence> tokenize(std::string_view, const TokenizationLimits&) const override {
            return siftwing::base::Result<TokenSequence>::success({"other"});
        }
    };
    bool destroyed = false;
    {
        std::unique_ptr<siftwing::text::Tokenizer> polymorphic = std::make_unique<OtherTokenizer>(destroyed);
        check(polymorphic->tokenize("", generous).value() == TokenSequence{"other"}, "abstract interface dispatches to an independent implementation");
    }
    check(destroyed, "base ownership invokes the derived destructor");
    const EnglishTokenizer tokenizer;
    for (unsigned i = 0; i < 30; ++i) {
        const auto repeated = tokenizer.tokenize("Ab 42 Ab", generous);
        check(repeated && repeated.value() == TokenSequence{"Ab","42","Ab"}, "const repeated calls carry no counters from previous calls");
        const auto failed = tokenizer.tokenize("long", {4,1,1,1});
        check(!failed, "interleaved failure does not publish or contaminate following calls");
    }
    const char* previous = std::setlocale(LC_CTYPE, nullptr);
    const std::string saved = previous ? previous : "C";
    for (const char* locale : {"C", "C.UTF-8"}) {
        if (std::setlocale(LC_CTYPE, locale)) { expected("I é 中文 42", {"I","42"}, "locale cannot extend ASCII classification or fold case"); }
    }
    check(std::setlocale(LC_CTYPE, saved.c_str()) != nullptr, "test restores original locale");
}
} // namespace

int main() {
    try { rules(); encoding_and_limits(); ownership_and_polymorphism(); }
    catch (const std::exception& e) { ++failures; std::cerr << "FAIL: unexpected exception: " << e.what() << '\n'; }
    std::cout << "English tokenizer checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
