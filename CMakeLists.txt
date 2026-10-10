cmake_minimum_required(VERSION 3.20)
project(siftwing LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SIFTWING_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(siftwing_build_options INTERFACE)
target_compile_options(siftwing_build_options INTERFACE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>"
)
if(SIFTWING_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR "SIFTWING_ENABLE_SANITIZERS requires GCC or Clang")
    endif()
    target_compile_options(siftwing_build_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(siftwing_build_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(siftwing_base INTERFACE)
target_include_directories(siftwing_base INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(siftwing_base INTERFACE siftwing_build_options)

add_library(siftwing_document INTERFACE)
target_link_libraries(siftwing_document INTERFACE siftwing_base)

add_library(siftwing_document_reader STATIC src/document/document_reader.cpp)
target_link_libraries(siftwing_document_reader PUBLIC siftwing_document)

# 固定官方归档及哈希；只使用头文件，避免引入上游安装规则或测试项目。
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(utf8cpp
    URL https://codeload.github.com/nemtrif/utfcpp/tar.gz/refs/tags/v4.0.6
    URL_HASH SHA256=6920a6a5d6a04b9a89b2a89af7132f8acefd46e0c2a7b190350539e9213816c0
    SOURCE_SUBDIR source
)
FetchContent_MakeAvailable(utf8cpp)
add_library(siftwing_utf8cpp INTERFACE)
target_include_directories(siftwing_utf8cpp SYSTEM INTERFACE "${utf8cpp_SOURCE_DIR}/source")

# 共用规范化只依赖基础结果/受检运算及固定版本UTF-8库，不耦合输入适配器。
add_library(siftwing_text_normalization STATIC src/text/normalize.cpp)
target_link_libraries(siftwing_text_normalization PUBLIC siftwing_base PRIVATE siftwing_utf8cpp)

# 分词接口不暴露第三方类型；英文实现与规范化解耦，调用方显式组装。
add_library(siftwing_tokenizer STATIC src/text/english_tokenizer.cpp)
target_link_libraries(siftwing_tokenizer PUBLIC siftwing_base PRIVATE siftwing_utf8cpp)

# 固定cppjieba及其limonp子模块；不运行上游安装/测试，也不加载关键词资源。
FetchContent_Declare(cppjieba
    URL https://codeload.github.com/yanyiwu/cppjieba/tar.gz/refs/tags/v5.6.7
    URL_HASH SHA256=08322f7c2a06a88e52eb0fdbc286ec86a85abae5b2a33b1e1430dba50b71960e
    SOURCE_SUBDIR source
)
FetchContent_Declare(limonp
    URL https://codeload.github.com/yanyiwu/limonp/tar.gz/9d74077dfcdf8073536c97a00bb79d7a3c3fdaba
    URL_HASH SHA256=2fff67ade507900ab7acb3c630b7b91232ff882f1b6240990875056befaa731b
    SOURCE_SUBDIR source
)
FetchContent_MakeAvailable(cppjieba limonp)
include(third_party/patches/cppjieba_memory.cmake)
siftwing_prepare_cppjieba("${cppjieba_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/cppjieba-memory/include")
add_library(siftwing_cppjieba INTERFACE)
target_include_directories(siftwing_cppjieba SYSTEM INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}/cppjieba-memory/include" "${limonp_SOURCE_DIR}/include")
add_library(siftwing_cppjieba_tokenizer STATIC src/text/cppjieba_tokenizer.cpp)
target_link_libraries(siftwing_cppjieba_tokenizer PUBLIC siftwing_tokenizer PRIVATE siftwing_cppjieba siftwing_utf8cpp)

# 精确停用词与词频只接收内存token/配置，不耦合文件装载或生产管线。
add_library(siftwing_term_frequency STATIC src/text/term_frequency.cpp)
target_link_libraries(siftwing_term_frequency PUBLIC siftwing_tokenizer PRIVATE siftwing_text_normalization siftwing_utf8cpp)

# 建库与查询共享拥有型管线，生产目标只组合文本阶段，不耦合文档读取或具体中文实现。
add_library(siftwing_text_pipeline STATIC src/text/text_pipeline.cpp)
target_link_libraries(siftwing_text_pipeline PUBLIC siftwing_text_normalization siftwing_term_frequency)

add_library(siftwing_txt_reader STATIC src/document/txt_reader.cpp)
target_link_libraries(siftwing_txt_reader PUBLIC siftwing_document_reader PRIVATE siftwing_utf8cpp)

# 固定Lexbor归档；只构建HTML及其core/dom/ns/tag依赖，不执行上游安装或其他模块。
FetchContent_Declare(lexbor
    URL https://codeload.github.com/lexbor/lexbor/tar.gz/refs/tags/v3.0.0
    URL_HASH SHA256=eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2
    SOURCE_SUBDIR source
)
FetchContent_MakeAvailable(lexbor)
set(siftwing_lexbor_sources)
foreach(module core dom html ns tag)
    file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "${lexbor_SOURCE_DIR}/source/lexbor/${module}/*.c")
    list(APPEND siftwing_lexbor_sources ${module_sources})
endforeach()
file(GLOB_RECURSE lexbor_ports CONFIGURE_DEPENDS "${lexbor_SOURCE_DIR}/source/lexbor/ports/posix/*.c")
add_library(siftwing_lexbor STATIC ${siftwing_lexbor_sources} ${lexbor_ports})
target_include_directories(siftwing_lexbor SYSTEM PUBLIC "${lexbor_SOURCE_DIR}/source")
target_compile_definitions(siftwing_lexbor PUBLIC LEXBOR_STATIC PRIVATE _POSIX_C_SOURCE=199309L)
target_link_libraries(siftwing_lexbor PRIVATE m)
if(SIFTWING_ENABLE_SANITIZERS)
    # 解析库本体也插桩；项目自身的五警告选项不施加到未修改的上游C代码。
    target_compile_options(siftwing_lexbor PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_library(siftwing_html_text STATIC src/document/html_text.cpp)
target_link_libraries(siftwing_html_text PUBLIC siftwing_base PRIVATE siftwing_lexbor siftwing_utf8cpp)

# 固定tinyxml2官方归档；直接编译单实现，不执行上游安装、工具或测试。
FetchContent_Declare(tinyxml2
    URL https://codeload.github.com/leethomason/tinyxml2/tar.gz/refs/tags/11.0.0
    URL_HASH SHA256=5556deb5081fb246ee92afae73efd943c889cef0cafea92b0b82422d6a18f289
    SOURCE_SUBDIR source
)
FetchContent_MakeAvailable(tinyxml2)
add_library(siftwing_tinyxml2 STATIC "${tinyxml2_SOURCE_DIR}/tinyxml2.cpp")
target_include_directories(siftwing_tinyxml2 SYSTEM PUBLIC "${tinyxml2_SOURCE_DIR}")
if(SIFTWING_ENABLE_SANITIZERS)
    target_compile_options(siftwing_tinyxml2 PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
add_library(siftwing_rss_reader STATIC src/document/rss_reader.cpp)
target_link_libraries(siftwing_rss_reader PUBLIC siftwing_document_reader
    PRIVATE siftwing_tinyxml2 siftwing_utf8cpp siftwing_html_text)

add_library(siftwing_bootstrap STATIC src/bootstrap/byte_probe.cpp)
target_include_directories(siftwing_bootstrap PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(siftwing_bootstrap PUBLIC siftwing_build_options)

add_executable(siftwing_build_probe apps/build_probe/main.cpp)
target_link_libraries(siftwing_build_probe PRIVATE siftwing_bootstrap)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_executable(siftwing_byte_probe_test tests/unit/byte_probe_test.cpp)
    target_link_libraries(siftwing_byte_probe_test PRIVATE siftwing_bootstrap)
    add_test(NAME bootstrap.byte_probe COMMAND siftwing_byte_probe_test)
    add_test(NAME bootstrap.cli
        COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:siftwing_build_probe>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/build_probe_cli.cmake"
    )
    set_tests_properties(bootstrap.byte_probe bootstrap.cli PROPERTIES TIMEOUT 10)
    add_test(NAME fixtures.integrity
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/golden_fixtures_test.py"
            --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden"
            --project-license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    )
    set_tests_properties(fixtures.integrity PROPERTIES TIMEOUT 15)

    add_executable(siftwing_result_test tests/unit/result_test.cpp)
    target_link_libraries(siftwing_result_test PRIVATE siftwing_base)
    add_test(NAME base.result COMMAND siftwing_result_test)
    set_tests_properties(base.result PROPERTIES TIMEOUT 10)
    add_executable(siftwing_checked_test tests/unit/checked_test.cpp)
    target_link_libraries(siftwing_checked_test PRIVATE siftwing_base)
    add_test(NAME base.checked COMMAND siftwing_checked_test)
    set_tests_properties(base.checked PROPERTIES TIMEOUT 30)
    add_executable(siftwing_id_test tests/unit/id_test.cpp)
    target_link_libraries(siftwing_id_test PRIVATE siftwing_base)
    add_test(NAME base.id COMMAND siftwing_id_test)
    set_tests_properties(base.id PROPERTIES TIMEOUT 10)
    add_executable(siftwing_document_record_test tests/unit/document_record_test.cpp)
    target_link_libraries(siftwing_document_record_test PRIVATE siftwing_document)
    add_test(NAME document.record COMMAND siftwing_document_record_test)
    set_tests_properties(document.record PROPERTIES TIMEOUT 10)
    add_executable(siftwing_document_reader_test tests/unit/document_reader_test.cpp)
    target_link_libraries(siftwing_document_reader_test PRIVATE siftwing_document_reader)
    target_compile_definitions(siftwing_document_reader_test PRIVATE
        SIFTWING_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden")
    add_test(NAME document.reader COMMAND siftwing_document_reader_test)
    set_tests_properties(document.reader PROPERTIES TIMEOUT 10)
    add_executable(siftwing_txt_reader_test tests/unit/txt_reader_test.cpp)
    target_link_libraries(siftwing_txt_reader_test PRIVATE siftwing_txt_reader)
    add_test(NAME document.txt_reader COMMAND siftwing_txt_reader_test)
    set_tests_properties(document.txt_reader PROPERTIES TIMEOUT 15)
    # 显式文件接入验收探针；公开CTest只生成原创输入，不依赖私有语料。
    add_executable(siftwing_txt_corpus_probe tests/integration/txt_corpus_probe.cpp)
    target_link_libraries(siftwing_txt_corpus_probe PRIVATE siftwing_txt_reader)
    add_test(NAME document.txt_file_ingestion
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/txt_file_ingestion_test.py"
            --probe "$<TARGET_FILE:siftwing_txt_corpus_probe>")
    set_tests_properties(document.txt_file_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_html_text_test tests/unit/html_text_test.cpp)
    target_link_libraries(siftwing_html_text_test PRIVATE siftwing_html_text)
    add_test(NAME document.html_text COMMAND siftwing_html_text_test)
    set_tests_properties(document.html_text PROPERTIES TIMEOUT 20)
    add_executable(siftwing_html_fragment_probe tests/integration/html_fragment_probe.cpp)
    target_link_libraries(siftwing_html_fragment_probe PRIVATE siftwing_html_text)
    add_test(NAME document.html_fragment_integration
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/html_fragment_integration_test.py"
            --probe "$<TARGET_FILE:siftwing_html_fragment_probe>"
            --rss "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden/rss/original.xml")
    set_tests_properties(document.html_fragment_integration PROPERTIES TIMEOUT 30)
    add_executable(siftwing_rss_reader_test tests/unit/rss_reader_test.cpp)
    target_link_libraries(siftwing_rss_reader_test PRIVATE siftwing_rss_reader)
    add_test(NAME document.rss_reader COMMAND siftwing_rss_reader_test)
    set_tests_properties(document.rss_reader PROPERTIES TIMEOUT 30)
    add_executable(siftwing_rss_corpus_probe tests/integration/rss_corpus_probe.cpp)
    target_link_libraries(siftwing_rss_corpus_probe PRIVATE siftwing_rss_reader)
    add_test(NAME document.rss_ingestion
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/rss_ingestion_test.py"
            --probe "$<TARGET_FILE:siftwing_rss_corpus_probe>"
            --rss "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden/rss/original.xml")
    set_tests_properties(document.rss_ingestion PROPERTIES TIMEOUT 30)
    add_executable(siftwing_normalize_test tests/unit/normalize_test.cpp)
    target_link_libraries(siftwing_normalize_test PRIVATE siftwing_text_normalization)
    add_test(NAME text.normalization COMMAND siftwing_normalize_test)
    set_tests_properties(text.normalization PROPERTIES TIMEOUT 15)
    add_executable(siftwing_normalize_probe tests/integration/normalize_probe.cpp)
    target_link_libraries(siftwing_normalize_probe PRIVATE siftwing_text_normalization)
    add_test(NAME text.normalization_reference
        COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/normalize_reference_test.py"
            --probe "$<TARGET_FILE:siftwing_normalize_probe>")
    set_tests_properties(text.normalization_reference PROPERTIES TIMEOUT 60)
    add_executable(siftwing_normalization_ingestion_test tests/integration/text_normalization_ingestion_test.cpp)
    target_link_libraries(siftwing_normalization_ingestion_test PRIVATE
        siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.normalization_ingestion COMMAND siftwing_normalization_ingestion_test)
    set_tests_properties(text.normalization_ingestion PROPERTIES TIMEOUT 15)
    add_executable(siftwing_english_tokenizer_test tests/unit/english_tokenizer_test.cpp)
    target_link_libraries(siftwing_english_tokenizer_test PRIVATE siftwing_tokenizer)
    add_test(NAME text.english_tokenizer COMMAND siftwing_english_tokenizer_test)
    set_tests_properties(text.english_tokenizer PROPERTIES TIMEOUT 15)
    add_executable(siftwing_english_tokenizer_probe tests/integration/english_tokenizer_probe.cpp)
    target_link_libraries(siftwing_english_tokenizer_probe PRIVATE siftwing_tokenizer)
    add_test(NAME text.english_tokenizer_reference
        COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/english_tokenizer_reference_test.py"
            --probe "$<TARGET_FILE:siftwing_english_tokenizer_probe>")
    set_tests_properties(text.english_tokenizer_reference PROPERTIES TIMEOUT 60)
    add_executable(siftwing_english_tokenizer_ingestion_test tests/integration/english_tokenizer_ingestion_test.cpp)
    target_link_libraries(siftwing_english_tokenizer_ingestion_test PRIVATE
        siftwing_tokenizer siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.english_tokenizer_ingestion COMMAND siftwing_english_tokenizer_ingestion_test)
    set_tests_properties(text.english_tokenizer_ingestion PROPERTIES TIMEOUT 15)
    add_executable(siftwing_cppjieba_tokenizer_test tests/unit/cppjieba_tokenizer_test.cpp)
    target_link_libraries(siftwing_cppjieba_tokenizer_test PRIVATE siftwing_cppjieba_tokenizer)
    add_test(NAME text.cppjieba_tokenizer COMMAND siftwing_cppjieba_tokenizer_test)
    add_executable(siftwing_cppjieba_tokenizer_probe tests/integration/cppjieba_tokenizer_probe.cpp)
    target_link_libraries(siftwing_cppjieba_tokenizer_probe PRIVATE siftwing_cppjieba_tokenizer)
    add_test(NAME text.cppjieba_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cppjieba_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_cppjieba_tokenizer_probe>")
    add_executable(siftwing_cppjieba_ingestion_test tests/integration/cppjieba_tokenizer_ingestion_test.cpp)
    target_link_libraries(siftwing_cppjieba_ingestion_test PRIVATE siftwing_cppjieba_tokenizer
        siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.cppjieba_ingestion COMMAND siftwing_cppjieba_ingestion_test)
    set_tests_properties(text.cppjieba_tokenizer text.cppjieba_reference text.cppjieba_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_term_frequency_test tests/unit/term_frequency_test.cpp)
    target_link_libraries(siftwing_term_frequency_test PRIVATE siftwing_term_frequency)
    add_test(NAME text.term_frequency COMMAND siftwing_term_frequency_test)
    add_executable(siftwing_term_frequency_probe tests/integration/term_frequency_probe.cpp)
    target_link_libraries(siftwing_term_frequency_probe PRIVATE siftwing_term_frequency)
    add_test(NAME text.term_frequency_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/term_frequency_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_term_frequency_probe>")
    add_executable(siftwing_term_frequency_ingestion_test tests/integration/term_frequency_ingestion_test.cpp)
    target_link_libraries(siftwing_term_frequency_ingestion_test PRIVATE siftwing_term_frequency
        siftwing_cppjieba_tokenizer siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.term_frequency_ingestion COMMAND siftwing_term_frequency_ingestion_test)
    set_tests_properties(text.term_frequency text.term_frequency_reference text.term_frequency_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_text_pipeline_test tests/unit/text_pipeline_test.cpp)
    target_link_libraries(siftwing_text_pipeline_test PRIVATE siftwing_text_pipeline)
    add_test(NAME text.pipeline COMMAND siftwing_text_pipeline_test)
    add_executable(siftwing_text_pipeline_ingestion_test tests/integration/text_pipeline_ingestion_test.cpp)
    target_link_libraries(siftwing_text_pipeline_ingestion_test PRIVATE siftwing_text_pipeline
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.pipeline_ingestion COMMAND siftwing_text_pipeline_ingestion_test)
    set_tests_properties(text.pipeline text.pipeline_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_text_pipeline_probe tests/integration/text_pipeline_probe.cpp)
    target_link_libraries(siftwing_text_pipeline_probe PRIVATE siftwing_text_pipeline siftwing_cppjieba_tokenizer)
    add_test(NAME text.pipeline_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/text_pipeline_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_text_pipeline_probe>")
    set_tests_properties(text.pipeline_reference PROPERTIES TIMEOUT 120)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        add_test(NAME text.pipeline_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/text-pipeline-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/text_pipeline_compile_contract.cmake")
        set_tests_properties(text.pipeline_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.term_frequency_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/term-frequency-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/term_frequency_compile_contract.cmake")
        set_tests_properties(text.term_frequency_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.cppjieba_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/cppjieba-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cppjieba_compile_contract.cmake")
        set_tests_properties(text.cppjieba_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.tokenizer_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/tokenizer-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tokenizer_compile_contract.cmake")
        set_tests_properties(text.tokenizer_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.normalization_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/normalization-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/normalize_compile_contract.cmake")
        set_tests_properties(text.normalization_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.rss_reader_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/rss-reader-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/rss_reader_compile_contract.cmake")
        set_tests_properties(document.rss_reader_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.html_text_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/html-text-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/html_text_compile_contract.cmake")
        set_tests_properties(document.html_text_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.txt_reader_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/txt-reader-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/txt_reader_compile_contract.cmake")
        set_tests_properties(document.txt_reader_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.reader_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/document-reader-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/document_reader_compile_contract.cmake")
        set_tests_properties(document.reader_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME base.result_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/result-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/result_compile_contract.cmake")
        set_tests_properties(base.result_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME base.checked_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/checked-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checked_compile_contract.cmake")
        set_tests_properties(base.checked_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME base.id_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/id-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/id_compile_contract.cmake")
        set_tests_properties(base.id_compile_contract PROPERTIES TIMEOUT 60)
        add_test(NAME document.record_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/document-record-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/document_record_compile_contract.cmake")
        set_tests_properties(document.record_compile_contract PROPERTIES TIMEOUT 30)
    endif()
endif()
