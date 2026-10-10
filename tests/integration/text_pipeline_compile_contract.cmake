if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/text/text_pipeline.h>\n${code}\n")
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
# 公共头独立自足，不暴露第三方；独占拥有/结果不可忽略由编译器验证。
check_compile(valid TRUE "" [=[
#include <siftwing/text/english_tokenizer.h>
#include <type_traits>
int main() {
    using namespace siftwing::text;
    static_assert(!std::is_default_constructible_v<TextPipeline>);
    static_assert(!std::is_copy_constructible_v<TextPipeline>);
    static_assert(!std::is_move_constructible_v<TextPipeline>);
    auto stops=StopWords::parse({}, {1,1,1,1});
    auto created=TextPipeline::create(std::make_unique<EnglishTokenizer>(),std::move(stops).value());
    auto analysis=created.value()->analyze("",{{1,1},{1,1,1,1},{1,1,1,1,1,1,1}});
    static_assert(std::is_same_v<decltype(analysis),siftwing::base::Result<TokenAnalysis>>);
}
]=])
check_compile(discard_create FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(siftwing::text::StopWords s) { siftwing::text::TextPipeline::create({},std::move(s)); }
]=])
check_compile(discard_analyze FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::text::TextPipeline& p) { p.analyze("",{{1,1},{1,1,1,1},{1,1,1,1,1,1,1}}); }
]=])
check_compile(copy_pipeline FALSE "(deleted|delete)" [=[
void call(const siftwing::text::TextPipeline& p) { auto other=p; (void)other; }
]=])
message(STATUS "4 text-pipeline compile contract cases passed")
