if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/text/normalize.h>\n${code}\n")
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
# 公共头不依赖第三方include；返回值按Result合同拥有string，借用与拥有访问均可静态定位。
check_compile(valid TRUE "" [=[
#include <type_traits>
int main() {
    using namespace siftwing::text;
    static_assert(normalization_policy_version == 1);
    auto result = normalize_utf8("A", {10,10});
    static_assert(std::is_same_v<decltype(result), siftwing::base::Result<std::string>>);
    static_assert(std::is_same_v<decltype(std::move(result).value()), std::string>);
    if (result) { const std::string& value = result.value(); (void)value; }
}
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
int main() { siftwing::text::normalize_utf8("A", {10,10}); }
]=])
check_compile(implicit_string FALSE "(conversion|convert|matching|candidate)" [=[
int main() { std::string text = siftwing::text::normalize_utf8("A", {10,10}); (void)text; }
]=])
message(STATUS "3 normalization compile contract cases passed (1 valid, 2 expected rejections)")
