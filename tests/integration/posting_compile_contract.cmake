if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cc")
    file(WRITE "${source}" "#include <siftwing/index/posting.hpp>\n${code}\n")
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
# 公共头自足、强文档身份和源整数类型约束；nodiscard拒绝无视失败。
check_compile(valid TRUE "" [=[
using namespace siftwing::index;
static_assert(!std::is_default_constructible_v<Posting>);
static_assert(!std::is_aggregate_v<Posting>);
static_assert(std::is_nothrow_copy_constructible_v<Posting>);
static_assert(std::is_nothrow_move_assignable_v<Posting>);
static_assert(std::is_same_v<decltype(std::declval<const Posting>().document_id()),siftwing::base::DocumentId>);
static_assert(std::is_same_v<decltype(std::declval<const Posting>().term_frequency()),std::uint64_t>);
void f(siftwing::base::DocumentId d) { auto p=Posting::from_count(d,1); (void)p; }
]=])
check_compile(default FALSE "(deleted|delete)" [=[
void f() { siftwing::index::Posting p; (void)p; }
]=])
check_compile(direct FALSE "(private|inaccessible)" [=[
void f(siftwing::base::DocumentId d) { siftwing::index::Posting p{d,0}; (void)p; }
]=])
check_compile(wrong_id FALSE "(matching|convert|conversion)" [=[
void f(siftwing::base::TermId d) { auto p=siftwing::index::Posting::from_count(d,1); (void)p; }
]=])
check_compile(raw_id FALSE "(matching|convert|conversion)" [=[
void f() { auto p=siftwing::index::Posting::from_count(0,1); (void)p; }
]=])
check_compile(boolean FALSE "(static assertion|static_assert)" [=[
void f(siftwing::base::DocumentId d) { auto p=siftwing::index::Posting::from_count(d,true); (void)p; }
]=])
check_compile(floating FALSE "(static assertion|static_assert)" [=[
void f(siftwing::base::DocumentId d) { auto p=siftwing::index::Posting::from_count(d,1.0); (void)p; }
]=])
check_compile(enumeration FALSE "(static assertion|static_assert)" [=[
enum Count { one=1 };
void f(siftwing::base::DocumentId d) { auto p=siftwing::index::Posting::from_count(d,one); (void)p; }
]=])
check_compile(discard_factory FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f(siftwing::base::DocumentId d) { siftwing::index::Posting::from_count(d,1); }
]=])
check_compile(discard_validation FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f() { siftwing::index::validate_posting_list({}, {1}); }
]=])
message(STATUS "10 posting compile contract cases passed")
