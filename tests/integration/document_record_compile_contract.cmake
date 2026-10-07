if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")

function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/document/document_record.h>\n${code}\n")
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

# 合法对照同时实例化公共记录、拥有型元数据及值赋值，排除头文件/编译参数的偶然失败。
check_compile(valid TRUE "" [=[
int main() {
    using namespace siftwing::document;
    auto id = siftwing::base::DocumentId::from_integer(0);
    DocumentRecord record{id.value(), "title", "content",
        {SourceKind::txt, {"input.txt", 0}, {}, {}, {}}};
    record.source.author = "author";
    auto copy = record;
    copy = record;
    return copy.doc_id.value() == 0 ? 0 : 1;
}
]=])

# 每个禁止操作独立编译并匹配拒绝原因；其他身份和来源序号不能冒充内部文档身份。
check_compile(default_record FALSE "(deleted|delete)" [=[
int main() { siftwing::document::DocumentRecord record; }
]=])
check_compile(raw_identity FALSE "(conversion|convert)" [=[
int main() { siftwing::document::DocumentRecord record{7, "", "", {}}; }
]=])
check_compile(term_identity FALSE "(conversion|convert)" [=[
int main() { auto id = siftwing::base::TermId::from_integer(7);
    siftwing::document::DocumentRecord record{id.value(), "", "", {}}; }
]=])
check_compile(snapshot_identity FALSE "(conversion|convert)" [=[
int main() { auto id = siftwing::base::SnapshotVersion::from_integer(7);
    siftwing::document::DocumentRecord record{id.value(), "", "", {}}; }
]=])
check_compile(identity_as_ordinal FALSE "(conversion|convert)" [=[
int main() { auto id = siftwing::base::DocumentId::from_integer(7);
    siftwing::document::SourceIdentity source{"input.txt", id.value()}; }
]=])
check_compile(integer_as_kind FALSE "(conversion|convert|incompatible type)" [=[
int main() { siftwing::document::DocumentSource source{}; source.kind = 1; }
]=])
message(STATUS "7 document record compile contract cases passed (1 valid, 6 expected rejections)")
