if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/document/html_text.h>\n${code}\n")
    execute_process(COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Werror=unused-result -fsyntax-only "-I${SOURCE_DIR}/include" "${source}"
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
# 公共合同只需项目include；返回拥有值，丢弃结果必须明确选择。
check_compile(valid TRUE "" [=[
#include <type_traits>
int main() {
    using namespace siftwing::document;
    static_assert(std::is_same_v<decltype(HtmlText::text), std::string>);
    auto result = extract_html_text("<p>x</p>", {32,32,32,32,8});
    if (!result) return 1;
    auto owned = std::move(result).value();
    return owned.text.empty() ? 1 : 0;
}
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
int main() { siftwing::document::extract_html_text("x", {32,32,32,32,8}); }
]=])
check_compile(borrow_temporary FALSE "(bind|reference)" [=[
int main() { auto& text = siftwing::document::extract_html_text("x", {32,32,32,32,8}).value(); (void)text; }
]=])
message(STATUS "3 HTML text compile contract cases passed (1 valid, 2 expected rejections)")
