if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")

function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/base/id.h>\n${code}\n")
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

# 合法对照排除 include/编译参数错误；排序与非排序两个身份域均真实实例化。
check_compile(valid TRUE "" [=[
using namespace siftwing::base;
void accepts_document(DocumentId) {}
int main() {
    auto document = DocumentId::from_integer(0);
    auto term = TermId::from_integer(7u);
    auto version = SnapshotVersion::from_integer(7);
    accepts_document(document.value());
    bool compared = document.value() <= document.value() && term.value() >= term.value()
        && version.value() == version.value();
    (void)DocumentId::from_integer(1);
    return compared && document && term && version ? 0 : 1;
}
]=])

# 每个禁止的公开操作独立编译，并匹配拒绝原因，不能把任意编译失败当作成功。
check_compile(default_document FALSE "(deleted|delete)" [=[
int main() { siftwing::base::DocumentId value; }
]=])
check_compile(default_term FALSE "(deleted|delete)" [=[
int main() { siftwing::base::TermId value; }
]=])
check_compile(default_version FALSE "(deleted|delete)" [=[
int main() { siftwing::base::SnapshotVersion value; }
]=])
check_compile(raw_constructor FALSE "private" [=[
int main() { siftwing::base::DocumentId value{7u}; }
]=])
check_compile(integer_to_id FALSE "(conversion|convert)" [=[
int main() { siftwing::base::TermId value = 7u; }
]=])
check_compile(id_to_integer FALSE "(conversion|convert)" [=[
int main() { auto value = siftwing::base::DocumentId::from_integer(7);
    std::uint64_t raw = value.value(); (void)raw; }
]=])
check_compile(cross_parameter FALSE "(conversion|convert)" [=[
void accepts_document(siftwing::base::DocumentId) {}
int main() { auto term = siftwing::base::TermId::from_integer(7);
    accepts_document(term.value()); }
]=])
check_compile(cross_assignment FALSE "(no match|no viable)" [=[
int main() { auto document = siftwing::base::DocumentId::from_integer(7);
    auto term = siftwing::base::TermId::from_integer(7);
    document.value() = term.value(); }
]=])
check_compile(cross_equality FALSE "(no match|invalid operands)" [=[
int main() { auto term = siftwing::base::TermId::from_integer(7);
    auto version = siftwing::base::SnapshotVersion::from_integer(7);
    return term.value() == version.value(); }
]=])
check_compile(cross_order FALSE "(no match|invalid operands)" [=[
int main() { auto document = siftwing::base::DocumentId::from_integer(7);
    auto term = siftwing::base::TermId::from_integer(7);
    return document.value() < term.value(); }
]=])
check_compile(version_order FALSE "(no match|invalid operands)" [=[
int main() { auto version = siftwing::base::SnapshotVersion::from_integer(7);
    return version.value() < version.value(); }
]=])
check_compile(version_less_equal FALSE "(no match|invalid operands)" [=[
int main() { auto version = siftwing::base::SnapshotVersion::from_integer(7);
    return version.value() <= version.value(); }
]=])
check_compile(version_greater FALSE "(no match|invalid operands)" [=[
int main() { auto version = siftwing::base::SnapshotVersion::from_integer(7);
    return version.value() > version.value(); }
]=])
check_compile(version_greater_equal FALSE "(no match|invalid operands)" [=[
int main() { auto version = siftwing::base::SnapshotVersion::from_integer(7);
    return version.value() >= version.value(); }
]=])
check_compile(id_arithmetic FALSE "(no match|invalid operands)" [=[
int main() { auto document = siftwing::base::DocumentId::from_integer(7);
    (void)(document.value() + document.value()); }
]=])
check_compile(id_increment FALSE "(no match|cannot increment)" [=[
int main() { auto term = siftwing::base::TermId::from_integer(7); ++term.value(); }
]=])
check_compile(bool_input FALSE "ID creation requires" [=[
int main() { (void)siftwing::base::DocumentId::from_integer(true); }
]=])
check_compile(float_input FALSE "ID creation requires" [=[
int main() { (void)siftwing::base::TermId::from_integer(7.0); }
]=])
check_compile(enum_input FALSE "ID creation requires" [=[
enum class Number : unsigned int { seven = 7 };
int main() { (void)siftwing::base::SnapshotVersion::from_integer(Number::seven); }
]=])
check_compile(id_input FALSE "ID creation requires" [=[
int main() { auto document = siftwing::base::DocumentId::from_integer(7);
    (void)siftwing::base::TermId::from_integer(document.value()); }
]=])
check_compile(discard_result FALSE "unused-result" [=[
int main() { siftwing::base::DocumentId::from_integer(7); }
]=])
message(STATUS "22 ID compile contract cases passed (1 valid, 21 expected rejections)")
