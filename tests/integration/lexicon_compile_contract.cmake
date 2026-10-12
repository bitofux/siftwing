if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cc")
    file(WRITE "${source}" "#include <siftwing/index/lexicon.hpp>\n${code}\n")
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
# 头自足/强身份/两种统计分离，借用不得从临时所有者取得，失败必须显式处理。
check_compile(valid TRUE "" [=[
#include <type_traits>
using namespace siftwing::index;
static_assert(!std::is_default_constructible_v<Lexicon>);
static_assert(!std::is_copy_constructible_v<Lexicon>);
static_assert(std::is_nothrow_move_constructible_v<Lexicon>);
static_assert(std::is_nothrow_move_assignable_v<Lexicon>);
static_assert(!std::is_copy_constructible_v<RecommendationDictionary>);
static_assert(std::is_nothrow_move_constructible_v<RecommendationDictionary>);
static_assert(std::is_nothrow_move_assignable_v<RecommendationDictionary>);
int main() { return 0; }
]=])
check_compile(copy FALSE "(deleted|delete)" [=[
void f(const siftwing::index::Lexicon &r) { auto copied=r; (void)copied; }
]=])
check_compile(temporary_terms FALSE "(deleted|delete)" [=[
void f(siftwing::index::Lexicon &r) { auto &v=std::move(r).terms(); (void)v; }
]=])
check_compile(temporary_term FALSE "(deleted|delete)" [=[
void f(siftwing::index::Lexicon &r,siftwing::base::TermId id) { auto v=std::move(r).term(id); (void)v; }
]=])
check_compile(temporary_dictionary FALSE "(deleted|delete)" [=[
void f(siftwing::index::RecommendationDictionary &r) { auto &v=std::move(r).lexicon(); (void)v; }
]=])
check_compile(temporary_characters FALSE "(deleted|delete)" [=[
void f(siftwing::index::RecommendationDictionary &r) { auto &v=std::move(r).characters(); (void)v; }
]=])
check_compile(temporary_frequencies FALSE "(deleted|delete)" [=[
void f(siftwing::index::RecommendationDictionary &r) { auto &v=std::move(r).frequencies(); (void)v; }
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f() { siftwing::index::Lexicon::build({},{}); }
]=])
check_compile(wrong_id FALSE "(convert|conversion|matching)" [=[
void f(const siftwing::index::Lexicon &r,siftwing::base::DocumentId id) { auto v=r.term(id); (void)v; }
]=])
check_compile(wrong_frequency_scope FALSE "(convert|conversion|matching|reference)" [=[
void f() { auto v=siftwing::index::RecommendationDictionary::build(siftwing::text::DocumentTermFrequencies{},{}); (void)v; }
]=])
message(STATUS "10 lexicon compile contract cases passed")
