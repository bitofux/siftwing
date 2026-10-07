if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")

function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/document/document_reader.h>\n${code}\n")
    execute_process(
        COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
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

# 合法实例覆盖覆写签名、虚析构、报告/借用输入与显式策略，不依赖链接或具体格式解析。
check_compile(valid TRUE "" [=[
struct Reader final : siftwing::document::DocumentReader {
    void read(const siftwing::document::ReaderInput&, const siftwing::document::ReadLimits&,
              siftwing::document::ReadSink&) override {}
};
int main() {
    Reader reader;
    const siftwing::document::ReadLimits limits{64,16,8,128};
    auto result = siftwing::document::read_documents({}, reader, limits, {});
    return result ? 0 : 1;
}
]=])
check_compile(abstract_reader FALSE "abstract" [=[
int main() { siftwing::document::DocumentReader reader; }
]=])
check_compile(abstract_sink FALSE "abstract" [=[
int main() { siftwing::document::ReadSink sink; }
]=])
check_compile(wrong_override FALSE "(override|overrid)" [=[
struct Reader : siftwing::document::DocumentReader {
    void read(siftwing::document::ReaderInput&, const siftwing::document::ReadLimits&,
              siftwing::document::ReadSink&) override {}
};
]=])
check_compile(ignored_emit FALSE "(nodiscard|unused-result|ignoring return value)" [=[
void submit(siftwing::document::ReadSink& sink, siftwing::document::ReadItem item) {
    sink.emit(item);
}
]=])
check_compile(ignored_report FALSE "(nodiscard|unused-result|ignoring return value)" [=[
void run(siftwing::document::DocumentReader& reader) {
    siftwing::document::read_documents({}, reader, {64,16,8,128}, {});
}
]=])
message(STATUS "6 document reader compile contract cases passed (1 valid, 5 expected rejections)")
