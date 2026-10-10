if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/document/rss_reader.h>\n${code}\n")
    execute_process(COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
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
# 公共头只依赖项目include；身份不能从整数隐式产生，多态适配器不能复制。
check_compile(valid TRUE "" [=[
#include <type_traits>
int main() {
    using namespace siftwing::document;
    static_assert(std::is_base_of_v<DocumentReader, RssDocumentReader>);
    RssDocumentReader reader({}, {10,10,{10,10,10,10,10}});
    DocumentReader& base = reader;
    (void)base;
}
]=])
check_compile(copy FALSE "(deleted|implicitly)" [=[
int main() { siftwing::document::RssDocumentReader r({}, {10,10,{10,10,10,10,10}}); auto copied = r; (void)copied; }
]=])
check_compile(integer_id FALSE "(conversion|convert|matching|candidate)" [=[
int main() { siftwing::document::RssDocumentReader r({{"feed.xml", {42}}}, {10,10,{10,10,10,10,10}}); }
]=])
message(STATUS "3 RSS compile contract cases passed (1 valid, 2 expected rejections)")
