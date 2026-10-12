if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/query/top_k.h>\n${code}\n")
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
# 自足公共头、拥有输出、强身份、nodiscard及禁止注入跨快照候选，预算不会改写Query。
check_compile(valid TRUE "" [=[
#include <type_traits>
using namespace siftwing;
static_assert(std::is_same_v<decltype(query::RankedDocument::document_id),base::DocumentId>);
static_assert(std::is_same_v<decltype(query::RankedDocument::score),double>);
static_assert(std::is_copy_constructible_v<query::RankedDocuments>);
void call(const query::Query &q, const index::WeightedIndex &i) {
    auto r=query::rank_top_k(q,i,{{{1,1,1},1},1}); (void)r;
}
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::query::Query &q, const siftwing::index::WeightedIndex &i) {
    siftwing::query::rank_top_k(q,i,{{{1,1,1},1},1});
}
]=])
check_compile(term_identity FALSE "(conversion|convert)" [=[
void call(siftwing::base::TermId id) { siftwing::query::RankedDocument out{id,1}; (void)out; }
]=])
check_compile(external_candidates FALSE "(arguments|reference|conversion|convert)" [=[
void call(const siftwing::query::Query &q, const siftwing::index::WeightedIndex &i,
          const siftwing::query::CandidateDocuments &c) {
    auto r=siftwing::query::rank_top_k(q,i,c,{{{1,1,1},1},1}); (void)r;
}
]=])
message(STATUS "4 top-k compile contract cases passed")
