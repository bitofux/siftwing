if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/query/query.h>\n${code}\n")
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
# 公共头自足；有效对象不能绕过工厂构造，临时对象不能交付TF借用。
check_compile(valid TRUE "" [=[
#include <type_traits>
using siftwing::query::Query;
static_assert(!std::is_default_constructible_v<Query>);
static_assert(std::is_copy_constructible_v<Query>);
static_assert(std::is_nothrow_move_constructible_v<Query>);
static_assert(!std::is_copy_assignable_v<Query> && !std::is_move_assignable_v<Query>);
static_assert(std::is_same_v<decltype(std::declval<const Query&>().frequencies()),
    const siftwing::text::DocumentTermFrequencies&>);
void call(const siftwing::text::TextPipeline& p, const siftwing::query::QueryLimits& l) {
    auto result=Query::from_text("red",p,{},l); (void)result;
}
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::text::TextPipeline& p, const siftwing::query::QueryLimits& l) {
    siftwing::query::Query::from_text("red",p,{},l);
}
]=])
check_compile(temporary_borrow FALSE "(deleted|delete)" [=[
void call(siftwing::query::Query q) { (void)std::move(q).frequencies(); }
]=])
check_compile(mutable_terms FALSE "(const|qualifier|conversion)" [=[
void call(siftwing::query::Query& q) { q.frequencies().terms.clear(); }
]=])
check_compile(default_query FALSE "(no matching|deleted|constructor)" [=[
void call() { siftwing::query::Query q; }
]=])
check_compile(assign_query FALSE "(deleted|delete)" [=[
void call(siftwing::query::Query& a, const siftwing::query::Query& b) { a=b; }
]=])
message(STATUS "6 query compile contract cases passed")
