if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cc")
    file(WRITE "${source}" "#include <siftwing/index/inverted_index.hpp>\n${code}\n")
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
# 只读借用拒绝临时所有者；DF/语料统计为值，推荐累计与搜索输入类型分离。
check_compile(valid TRUE "" [=[
using namespace siftwing::index;
static_assert(!std::is_default_constructible_v<InvertedIndex>);
static_assert(!std::is_copy_constructible_v<InvertedIndex>);
static_assert(!std::is_copy_assignable_v<InvertedIndex>);
static_assert(std::is_nothrow_move_constructible_v<InvertedIndex>);
static_assert(std::is_nothrow_move_assignable_v<InvertedIndex>);
static_assert(std::is_same_v<decltype(std::declval<InvertedIndex>().statistics()),CorpusStatistics>);
static_assert(std::is_same_v<decltype(std::declval<InvertedIndex>().document_frequency(std::declval<siftwing::base::TermId>())),std::optional<std::uint64_t>>);
void f(const InvertedIndex& v,siftwing::base::TermId t,siftwing::base::DocumentId d) {
    const auto& l=v.lexicon(); const auto& docs=v.documents(); const auto* p=v.postings(t); const auto* s=v.document(d);
    (void)l;(void)docs;(void)p;(void)s;
}
]=])
check_compile(copy FALSE "(deleted|delete)" [=[
void f(const siftwing::index::InvertedIndex& v) { auto copy=v; (void)copy; }
]=])
check_compile(temporary_lexicon FALSE "(deleted|delete)" [=[
void f(siftwing::index::InvertedIndex&& v) { const auto& a=std::move(v).lexicon(); (void)a; }
]=])
check_compile(temporary_documents FALSE "(deleted|delete)" [=[
void f(siftwing::index::InvertedIndex&& v) { const auto& a=std::move(v).documents(); (void)a; }
]=])
check_compile(temporary_postings FALSE "(deleted|delete)" [=[
void f(siftwing::index::InvertedIndex&& v,siftwing::base::TermId t) { const auto* a=std::move(v).postings(t); (void)a; }
]=])
check_compile(temporary_document FALSE "(deleted|delete)" [=[
void f(siftwing::index::InvertedIndex&& v,siftwing::base::DocumentId d) { const auto* a=std::move(v).document(d); (void)a; }
]=])
check_compile(wrong_term FALSE "(matching|convert|conversion)" [=[
void f(const siftwing::index::InvertedIndex& v,siftwing::base::DocumentId d) { auto p=v.postings(d); (void)p; }
]=])
check_compile(wrong_document FALSE "(matching|convert|conversion)" [=[
void f(const siftwing::index::InvertedIndex& v,siftwing::base::TermId t) { auto p=v.document(t); (void)p; }
]=])
check_compile(recommendation FALSE "(matching|convert|conversion)" [=[
void f(const siftwing::text::RecommendationFrequencies& r,const siftwing::index::InvertedIndexLimits& l) {
    auto p=siftwing::index::InvertedIndex::build(r,l); (void)p;
}
]=])
check_compile(discard_build FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f() { siftwing::index::InvertedIndex::build({},{}); }
]=])
check_compile(mutable_list FALSE "(const|qualifier|matching)" [=[
void f(const siftwing::index::InvertedIndex& v,siftwing::base::TermId t) { v.postings(t)->clear(); }
]=])
message(STATUS "11 inverted index compile contract cases passed")
