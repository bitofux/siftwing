if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/text/english_tokenizer.h>\n${code}\n")
    execute_process(COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror=unused-result
        -fsyntax-only "-I${SOURCE_DIR}/include" "${source}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${BINARY_DIR}/${name}.txt" "exit=${result}\n${output}${error}")
    if(should_pass)
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "${name} must compile:\n${output}${error}")
        endif()
    elseif(result STREQUAL "0" OR NOT error MATCHES "${diagnostic}")
        message(FATAL_ERROR "${name} expected rejection: exit=${result}\n${error}")
    endif()
endfunction()
# 无第三方include即可使用公共头；抽象/多态、const调用、拥有型Result都属于调用者合同。
check_compile(valid TRUE "" [=[
#include <memory>
#include <type_traits>
int main() {
    using namespace siftwing::text;
    static_assert(english_tokenization_policy_version == 1);
    static_assert(std::is_abstract_v<Tokenizer>);
    static_assert(std::has_virtual_destructor_v<Tokenizer>);
    static_assert(std::is_final_v<EnglishTokenizer>);
    static_assert(std::is_nothrow_destructible_v<Tokenizer>);
    const EnglishTokenizer english;
    const Tokenizer& base = english;
    auto result = base.tokenize("a1", {10,10,10,10});
    static_assert(std::is_same_v<decltype(result), siftwing::base::Result<TokenSequence>>);
    static_assert(std::is_same_v<decltype(std::move(result).value()), TokenSequence>);
    std::unique_ptr<Tokenizer> owned = std::make_unique<EnglishTokenizer>();
    (void)owned;
}
]=])
check_compile(discard_concrete FALSE "(nodiscard|unused-result|ignoring return)" [=[
int main() { siftwing::text::EnglishTokenizer{}.tokenize("a", {1,1,1,1}); }
]=])
check_compile(discard_base FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::text::Tokenizer& t) { t.tokenize("a", {1,1,1,1}); }
]=])
check_compile(abstract FALSE "(abstract|pure virtual)" [=[
int main() { siftwing::text::Tokenizer tokenizer; }
]=])
check_compile(implicit_sequence FALSE "(conversion|convert|matching|candidate)" [=[
int main() { siftwing::text::TokenSequence words = siftwing::text::EnglishTokenizer{}.tokenize("a", {1,1,1,1}); (void)words; }
]=])
message(STATUS "5 tokenizer compile contract cases passed (1 valid, 4 expected rejections)")
