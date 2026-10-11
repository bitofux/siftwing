if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/document/document_store.h>\n${code}\n")
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
# 头自足、资源不可复制且可 noexcept 移动；临时对象不能泄露条目引用，强身份与失败必须显式处理。
check_compile(valid TRUE "" [=[
#include <type_traits>
using namespace siftwing::document;
static_assert(!std::is_default_constructible_v<DocumentStoreReader>);
static_assert(!std::is_copy_constructible_v<DocumentStoreReader>);
static_assert(!std::is_copy_assignable_v<DocumentStoreReader>);
static_assert(std::is_nothrow_move_constructible_v<DocumentStoreReader>);
static_assert(std::is_nothrow_move_assignable_v<DocumentStoreReader>);
static_assert(std::is_same_v<decltype(std::declval<const DocumentStoreReader&>().read(
    std::declval<siftwing::base::DocumentId>())), siftwing::base::Result<DocumentRecord>>);
int main() { return 0; }
]=])
check_compile(copy_reader FALSE "(deleted|delete)" [=[
void f(const siftwing::document::DocumentStoreReader &r) { auto copied=r; (void)copied; }
]=])
check_compile(temporary_entries FALSE "(deleted|delete)" [=[
#include <utility>
void f(siftwing::document::DocumentStoreReader &r) { auto &entries=std::move(r).entries(); (void)entries; }
]=])
check_compile(discard_read FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f(const siftwing::document::DocumentStoreReader &r,siftwing::base::DocumentId id) { r.read(id); }
]=])
check_compile(bare_identity FALSE "(convert|conversion|matching)" [=[
void f(const siftwing::document::DocumentStoreReader &r) { auto value=r.read(0); (void)value; }
]=])
message(STATUS "5 document store compile contract cases passed")
