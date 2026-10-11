if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/document/simhash.h>\n${code}\n")
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
# 公共头自足，距离明确64位值类型；不可忽略Result和距离。
check_compile(valid TRUE "" [=[
#include <type_traits>
using namespace siftwing::document;
static_assert(noexcept(hamming_distance({},{})));
static_assert(std::is_same_v<decltype(make_simhash({},{})),
    siftwing::base::Result<std::optional<SimHash64>>>);
int main() { return static_cast<int>(hamming_distance({0},{0})); }
]=])
check_compile(discard_result FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call() { siftwing::document::make_simhash({},{}); }
]=])
check_compile(discard_distance FALSE "(nodiscard|unused-result|ignoring return)" [=[
void call() { siftwing::document::hamming_distance({},{}); }
]=])
check_compile(bare_integer FALSE "(convert|conversion)" [=[
void call() { auto n=siftwing::document::hamming_distance(0,0); (void)n; }
]=])
message(STATUS "4 simhash compile contract cases passed")
