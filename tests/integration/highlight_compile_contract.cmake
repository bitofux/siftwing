if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/query/highlight.h>\n${code}\n")
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
# 公共头自足、HTML及区间拥有/可复制、nodiscard；拒绝用ranked结果或词身份代替Query。
check_compile(valid TRUE "" [=[
#include <type_traits>
using namespace siftwing;
static_assert(std::is_same_v<decltype(query::Highlight::html),std::string>);
static_assert(std::is_same_v<decltype(query::HighlightSpan::byte_offset),std::uint64_t>);
static_assert(std::is_copy_constructible_v<query::Highlight>);
static_assert(std::is_nothrow_move_constructible_v<query::Highlight>);
void call(const query::Query &q) { auto r=query::make_highlight(q,"正文",{100,10,100,1000,10,1000}); (void)r; }
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call(const siftwing::query::Query &q) { siftwing::query::make_highlight(q,"",{1,1,1,1,1,100}); }
]=])
check_compile(no_query FALSE "(reference|conversion|convert)" [=[
void call() { auto r=siftwing::query::make_highlight("term","body",{1,1,1,1,1,100}); (void)r; }
]=])
check_compile(term_id FALSE "(reference|conversion|convert)" [=[
#include <siftwing/base/id.h>
void call(siftwing::base::TermId id) { auto r=siftwing::query::make_highlight(id,"",{1,1,1,1,1,100}); (void)r; }
]=])
message(STATUS "4 highlight compile contract cases passed")
