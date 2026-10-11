if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/snapshot/manifest.h>\n#include <type_traits>\n${code}\n")
    execute_process(COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Werror=unused-result -fsyntax-only "-I${SOURCE_DIR}/include" "${source}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${BINARY_DIR}/${name}.txt" "exit=${result}\n${output}${error}")
    if(should_pass)
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "${name} must compile: ${error}")
        endif()
    elseif(result STREQUAL "0" OR NOT error MATCHES "${diagnostic}")
        message(FATAL_ERROR "${name} expected rejection: ${result}\n${error}")
    endif()
endfunction()
# 普通拥有值可独立复制；强快照身份和枚举角色不得隐式混用，返回不可忽略。
check_compile(valid TRUE "" [=[
using namespace siftwing::snapshot;
static_assert(std::is_copy_constructible_v<Manifest>);
static_assert(std::is_copy_assignable_v<Manifest>);
static_assert(!std::is_default_constructible_v<Manifest>);
static_assert(std::is_same_v<decltype(sha256_bytes("")),siftwing::base::Result<std::string>>);
static_assert(artifact_layout.size()==6);
]=])
check_compile(wrong_identity FALSE "(convert|conversion)" [=[
void f(siftwing::snapshot::Manifest& m,siftwing::base::DocumentId d){m.snapshot=d;}
]=])
check_compile(raw_identity FALSE "(convert|conversion)" [=[
void f(siftwing::snapshot::Manifest& m){m.snapshot=7;}
]=])
check_compile(wrong_role FALSE "(convert|conversion|initialize)" [=[
void f(const siftwing::snapshot::Manifest& m){auto r=siftwing::snapshot::verify_artifact(m,0,"",{});(void)r;}
]=])
check_compile(discard_parse FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f(){siftwing::snapshot::parse_manifest("",{});}
]=])
check_compile(discard_validate FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f(const siftwing::snapshot::Manifest& m){siftwing::snapshot::validate_manifest(m,{});}
]=])
check_compile(discard_hash FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f(){siftwing::snapshot::sha256_bytes("");}
]=])
message(STATUS "7 manifest compile contract cases passed")
