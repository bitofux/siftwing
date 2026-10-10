if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/text/cppjieba_tokenizer.h>\n${code}\n")
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
# 公共头不暴露cppjieba/limonp；Pimpl的析构和多态转换在无第三方include时合法。
check_compile(valid TRUE "" [=[
#include <type_traits>
int main() {
    using namespace siftwing::text;
    static_assert(std::is_final_v<CppJiebaTokenizer>);
    static_assert(!std::is_copy_constructible_v<CppJiebaTokenizer>);
    static_assert(!std::is_move_constructible_v<CppJiebaTokenizer>);
    static_assert(std::is_nothrow_destructible_v<CppJiebaTokenizer>);
    auto result = CppJiebaTokenizer::create({});
    static_assert(std::is_same_v<decltype(result), siftwing::base::Result<std::unique_ptr<CppJiebaTokenizer>>>);
    if(result) { std::unique_ptr<Tokenizer> polymorphic = std::move(result).value(); }
}
]=])
check_compile(discard_factory FALSE "(nodiscard|unused-result|ignoring return)" [=[
int main() { siftwing::text::CppJiebaTokenizer::create({}); }
]=])
check_compile(discard_tokens FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::text::CppJiebaTokenizer& t) { t.tokenize("a", {1,1,1,1}); }
]=])
check_compile(default_construct FALSE "(matching|candidate|deleted|private)" [=[
int main() { siftwing::text::CppJiebaTokenizer tokenizer; }
]=])
message(STATUS "4 cppjieba compile contract cases passed")
