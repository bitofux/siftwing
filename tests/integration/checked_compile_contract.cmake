if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")

function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/base/checked.h>\n${code}\n")
    execute_process(
        COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
            -Werror=unused-result -fsyntax-only "-I${SOURCE_DIR}/include" "${source}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${BINARY_DIR}/${name}.txt" "exit=${result}\n${output}${error}")
    if(should_pass)
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "${name} must compile:\n${output}${error}")
        endif()
    else()
        if(result STREQUAL "0" OR NOT error MATCHES "${diagnostic}")
            message(FATAL_ERROR "${name} expected rejection: exit=${result}\n${error}")
        endif()
    endif()
endfunction()

check_compile(valid TRUE "" [=[
int main() {
    auto sum = siftwing::base::checked_add(3, -1);
    auto product = siftwing::base::checked_mul(3u, 2u);
    auto field = siftwing::base::checked_narrow<unsigned short>(product.value());
    (void)siftwing::base::checked_add(0, 0);
    return sum && product && field ? 0 : 1;
}
]=])
check_compile(discard_add FALSE "unused-result" [=[
int main() { siftwing::base::checked_add(1u, 2u); }
]=])
check_compile(discard_mul FALSE "unused-result" [=[
int main() { siftwing::base::checked_mul(1, 2); }
]=])
check_compile(discard_narrow FALSE "unused-result" [=[
int main() { siftwing::base::checked_narrow<unsigned int>(1); }
]=])
check_compile(bool_arithmetic FALSE "checked arithmetic requires" [=[
int main() { (void)siftwing::base::checked_add(true, false); }
]=])
check_compile(bool_conversion FALSE "checked conversion requires" [=[
int main() { (void)siftwing::base::checked_narrow<bool>(1); }
]=])
check_compile(float_conversion FALSE "checked conversion requires" [=[
int main() { (void)siftwing::base::checked_narrow<int>(1.5); }
]=])
check_compile(mixed_arithmetic FALSE "(no matching|no viable)" [=[
int main() { (void)siftwing::base::checked_mul(1u, -1); }
]=])
message(STATUS "8 compile contract cases passed (1 valid, 7 expected rejections)")
