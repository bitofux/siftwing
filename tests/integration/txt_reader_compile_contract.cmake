if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")

function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/document/txt_reader.h>\n${code}\n")
    execute_process(
        COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
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

# 公共头可独立使用项目include目录；不要求调用者直接包含utfcpp头文件。
check_compile(valid TRUE "" [=[
int main() {
    auto id = siftwing::base::DocumentId::from_integer(0);
    siftwing::document::TxtDocumentReader reader({{"a.txt", {id.value(), std::nullopt}}});
    siftwing::document::DocumentReader& base = reader;
    (void)base;
}
]=])
check_compile(raw_id FALSE "(convert|conversion|matching)" [=[
int main() { siftwing::document::TxtDocumentMetadata metadata{0, std::nullopt}; }
]=])
check_compile(copy_reader FALSE "(deleted|delete)" [=[
void run(const siftwing::document::TxtDocumentReader& reader) { auto copy = reader; }
]=])
check_compile(derive_final FALSE "final" [=[
struct Derived : siftwing::document::TxtDocumentReader {};
]=])
message(STATUS "4 TXT reader compile contract cases passed (1 valid, 3 expected rejections)")
