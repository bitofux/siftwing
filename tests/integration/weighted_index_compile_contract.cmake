if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/index/weighted_index.h>\n${code}\n")
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
# 只读借用拒绝临时所有者；范数/IDF为值，原始索引必须显式转交，推荐类型拒绝。
check_compile(valid TRUE "" [=[
using namespace siftwing::index;
static_assert(!std::is_default_constructible_v<WeightedIndex>);
static_assert(!std::is_copy_constructible_v<WeightedIndex>);
static_assert(!std::is_copy_assignable_v<WeightedIndex>);
static_assert(std::is_nothrow_move_constructible_v<WeightedIndex>);
static_assert(std::is_nothrow_move_assignable_v<WeightedIndex>);
static_assert(std::is_same_v<decltype(std::declval<WeightedIndex>().document_norm(std::declval<siftwing::base::DocumentId>())),std::optional<double>>);
void f(const WeightedIndex& v,siftwing::base::TermId t) {
    const auto& raw=v.raw_index(); const auto* w=v.weights(t); (void)raw;(void)w;
}
]=])
check_compile(copy FALSE "(deleted|delete)" [=[
void f(const siftwing::index::WeightedIndex& v) { auto copy=v; (void)copy; }
]=])
check_compile(temporary_raw FALSE "(deleted|delete)" [=[
void f(siftwing::index::WeightedIndex&& v) { const auto& a=std::move(v).raw_index(); (void)a; }
]=])
check_compile(temporary_weights FALSE "(deleted|delete)" [=[
void f(siftwing::index::WeightedIndex&& v,siftwing::base::TermId t) { const auto* a=std::move(v).weights(t); (void)a; }
]=])
check_compile(wrong_term FALSE "(matching|convert|conversion)" [=[
void f(const siftwing::index::WeightedIndex& v,siftwing::base::DocumentId d) { auto p=v.weights(d); (void)p; }
]=])
check_compile(wrong_document FALSE "(matching|convert|conversion)" [=[
void f(const siftwing::index::WeightedIndex& v,siftwing::base::TermId t) { auto p=v.document_norm(t); (void)p; }
]=])
check_compile(borrowed_input FALSE "(deleted|delete)" [=[
void f(siftwing::index::InvertedIndex& r,const siftwing::index::WeightLimits& l) {
    auto p=siftwing::index::WeightedIndex::build(r,l); (void)p;
}
]=])
check_compile(recommendation FALSE "(matching|convert|conversion)" [=[
void f(const siftwing::text::RecommendationFrequencies& r,const siftwing::index::WeightLimits& l) {
    auto p=siftwing::index::WeightedIndex::build(r,l); (void)p;
}
]=])
check_compile(discard_build FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f(siftwing::index::InvertedIndex&& r) { siftwing::index::WeightedIndex::build(std::move(r),{}); }
]=])
check_compile(discard_serialization FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f(const siftwing::index::WeightedIndex& v) { v.serialize({10}); }
]=])
check_compile(mutable_weights FALSE "(const|qualifier|matching)" [=[
void f(const siftwing::index::WeightedIndex& v,siftwing::base::TermId t) { v.weights(t)->clear(); }
]=])
message(STATUS "11 weighted index compile contract cases passed")
