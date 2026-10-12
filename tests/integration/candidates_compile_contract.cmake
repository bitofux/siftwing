if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/query/candidates.h>\n${code}\n")
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
# 公共头自足；返回拥有强文档身份，不能丢诊断或当作词身份集合。
check_compile(valid TRUE "" [=[
#include <type_traits>
using namespace siftwing;
static_assert(std::is_same_v<query::CandidateDocuments::value_type,base::DocumentId>);
static_assert(std::is_copy_constructible_v<query::CandidateDocuments>);
void call(const query::Query &q, const index::InvertedIndex &i) {
    auto r=query::collect_candidates(q,i,{1,1,1}); (void)r;
}
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::query::Query &q, const siftwing::index::InvertedIndex &i) {
    siftwing::query::collect_candidates(q,i,{1,1,1});
}
]=])
check_compile(term_identity FALSE "(no matching|conversion|convert)" [=[
void call(siftwing::base::TermId id) { siftwing::query::CandidateDocuments out; out.push_back(id); }
]=])
message(STATUS "3 candidate compile contract cases passed")
