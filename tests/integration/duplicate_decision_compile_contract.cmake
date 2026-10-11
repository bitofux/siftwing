if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/document/duplicate_decision.h>\n${code}\n")
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
# 自足头、强身份和显式缺席语义；函数返回值不可忽略，原文记录不隐式变成指纹候选。
check_compile(valid TRUE "" [=[
#include <type_traits>
using namespace siftwing::document;
static_assert(std::is_same_v<decltype(decide_duplicates({}, {}, {})),
    siftwing::base::Result<DuplicateReport>>);
static_assert(!std::is_default_constructible_v<DuplicateCandidate>);
static_assert(!std::is_default_constructible_v<DuplicateDecision>);
static_assert(!noexcept(decide_duplicates({}, {}, {})));
int main() {
    auto id = siftwing::base::DocumentId::from_integer(0).value();
    DuplicateCandidate candidate{id,{"a",0},SimHash64{0}};
    auto result = decide_duplicates({candidate},{},{1,1,1,1});
    return result ? 0 : 1;
}
]=])
check_compile(discard_result FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call() { siftwing::document::decide_duplicates({}, {}, {}); }
]=])
check_compile(bare_identity FALSE "(convert|conversion)" [=[
void call() { siftwing::document::DuplicateCandidate candidate{0,{"a",0},std::nullopt}; (void)candidate; }
]=])
check_compile(record_is_not_candidate FALSE "(convert|conversion|reference|matching)" [=[
void call(const std::vector<siftwing::document::DocumentRecord>& records) {
    auto result = siftwing::document::decide_duplicates(records, {}, {}); (void)result;
}
]=])
message(STATUS "4 duplicate decision compile contract cases passed")
