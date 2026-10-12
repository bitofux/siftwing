if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cc")
    file(WRITE "${source}" "#include <siftwing/query/recommendation.hpp>\n${code}\n")
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
# 公共头自足、结果词拥有、强推荐词ID；搜索倒排和文档身份不能替代相应参数。
check_compile(valid TRUE "" [=[
#include <type_traits>
using namespace siftwing;
static_assert(std::is_same_v<decltype(query::RecommendedTerm::term),std::string>);
static_assert(std::is_same_v<decltype(query::RecommendedTerm::term_id),base::TermId>);
static_assert(std::is_copy_constructible_v<query::RecommendedTerms>);
static_assert(std::is_nothrow_move_constructible_v<query::RecommendedTerms>);
void call(const index::RecommendationDictionary &d) {
    auto r=query::recommend_terms("词",d,1,{100,10,1,100,100,1000,10000,1000}); (void)r;
}
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::index::RecommendationDictionary &d) {
    siftwing::query::recommend_terms("a",d,1,{100,10,1,100,100,1000,10000,1000});
}
]=])
check_compile(search_dictionary FALSE "(reference|conversion|convert)" [=[
void call(const siftwing::index::Lexicon &d) {
    auto r=siftwing::query::recommend_terms("a",d,1,{}); (void)r;
}
]=])
check_compile(document_id FALSE "(reference|conversion|convert)" [=[
void call(const siftwing::index::RecommendationDictionary &d,siftwing::base::DocumentId id) {
    auto r=siftwing::query::recommend_terms("a",d,id,{}); (void)r;
}
]=])
message(STATUS "4 recommendation compile contract cases passed")
