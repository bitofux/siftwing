if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()

file(MAKE_DIRECTORY "${BINARY_DIR}")

function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/base/result.h>\n${code}\n")
    execute_process(
        COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
            -Werror=unused-result -fsyntax-only "-I${SOURCE_DIR}/include" "${source}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    )
    file(WRITE "${BINARY_DIR}/${name}.txt" "exit=${result}\n${output}${error}")
    if(should_pass)
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "${name} must compile:\n${output}${error}")
        endif()
    else()
        if(result STREQUAL "0")
            message(FATAL_ERROR "${name} unexpectedly compiled")
        endif()
        if(NOT error MATCHES "${diagnostic}")
            message(FATAL_ERROR "${name} failed for an unexpected reason:\n${error}")
        endif()
    endif()
endfunction()

check_compile(valid TRUE "" [=[
using siftwing::base::Result;
int main() {
    auto result = Result<int>::success(3);
    if (!result || result.value() != 3) return 1;
    auto done = Result<void>::success();
    done.value();
    (void)Result<int>::failure({"intentional", "discard"});
    return 0;
}
]=])

check_compile(discard_value FALSE "unused-result" [=[
siftwing::base::Result<int> operation() {
    return siftwing::base::Result<int>::success(3);
}
int main() { operation(); }
]=])

check_compile(discard_void FALSE "unused-result" [=[
siftwing::base::Result<void> operation() {
    return siftwing::base::Result<void>::success();
}
int main() { operation(); }
]=])

check_compile(assign_result FALSE "deleted" [=[
int main() {
    auto result = siftwing::base::Result<int>::success(1);
    result = siftwing::base::Result<int>::failure({"reason", "context"});
}
]=])

check_compile(default_result FALSE "(no matching|no viable)" [=[
int main() { siftwing::base::Result<int> result; }
]=])

check_compile(reference_value FALSE "requires a non-cv, non-array object type" [=[
int main() { (void)sizeof(siftwing::base::Result<int&>); }
]=])

message(STATUS "6 compile contract cases passed (1 valid, 5 expected rejections)")
