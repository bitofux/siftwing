if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/text/term_frequency.h>\n${code}\n")
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
# 两种统计类型显式分开，无第三方include也能消费接口；不能把文档TF当累计表。
check_compile(valid TRUE "" [=[
#include <type_traits>
int main() {
    using namespace siftwing::text;
    static_assert(!std::is_same_v<DocumentTermFrequencies,RecommendationFrequencies>);
    auto config=StopWords::parse({}, {1,1,1,1});
    auto analyzed=analyze_tokens({},config.value(),{1,1,1,1,1,1,1});
    auto sum=accumulate_recommendation({},analyzed.value().tf,{1,1,1,1,1});
    static_assert(std::is_same_v<decltype(sum),siftwing::base::Result<RecommendationFrequencies>>);
}
]=])
check_compile(discard_parse FALSE "(nodiscard|unused-result|ignoring return)" [=[
int main() { siftwing::text::StopWords::parse({}, {1,1,1,1}); }
]=])
check_compile(discard_analyze FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::text::StopWords& s) { siftwing::text::analyze_tokens({}, s, {1,1,1,1,1,1,1}); }
]=])
check_compile(wrong_scope FALSE "(conversion|convert|reference|matching|candidate)" [=[
int main() { auto r=siftwing::text::accumulate_recommendation(siftwing::text::DocumentTermFrequencies{}, {}, {1,1,1,1,1}); (void)r; }
]=])
message(STATUS "4 term-frequency compile contract cases passed")
