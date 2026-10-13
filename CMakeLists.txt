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

add_library(siftwing_buffer STATIC src/net/buffer.cc)
target_link_libraries(siftwing_buffer PUBLIC siftwing_base)

add_library(siftwing_frame_decoder STATIC src/net/frame_decoder.cc)
target_link_libraries(siftwing_frame_decoder PUBLIC siftwing_buffer)

add_library(siftwing_document INTERFACE)
target_link_libraries(siftwing_document INTERFACE siftwing_base)

add_library(siftwing_document_reader STATIC src/document/document_reader.cc)
target_link_libraries(siftwing_document_reader PUBLIC siftwing_document)

add_library(siftwing_record_format STATIC src/document/record_format.cc)
target_link_libraries(siftwing_record_format PUBLIC siftwing_document)

add_library(siftwing_document_store STATIC src/document/document_store.cc)
target_link_libraries(siftwing_document_store PUBLIC siftwing_record_format)

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
add_library(siftwing_text_normalization STATIC src/text/normalize.cc)
target_link_libraries(siftwing_text_normalization PUBLIC siftwing_base PRIVATE siftwing_utf8cpp)

# 分词接口不暴露第三方类型；英文实现与规范化解耦，调用方显式组装。
add_library(siftwing_tokenizer STATIC src/text/english_tokenizer.cc)
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
add_library(siftwing_cppjieba_tokenizer STATIC src/text/cppjieba_tokenizer.cc)
target_link_libraries(siftwing_cppjieba_tokenizer PUBLIC siftwing_tokenizer PRIVATE siftwing_cppjieba siftwing_utf8cpp)

# 精确停用词与词频只接收内存token/配置，不耦合文件装载或生产管线。
add_library(siftwing_term_frequency STATIC src/text/term_frequency.cc)
target_link_libraries(siftwing_term_frequency PUBLIC siftwing_tokenizer PRIVATE siftwing_text_normalization siftwing_utf8cpp)

# 固定词表与独立推荐字符索引不耦合文件装载、搜索Posting或推荐查询算法。
add_library(siftwing_lexicon STATIC src/index/lexicon.cc)
target_link_libraries(siftwing_lexicon PUBLIC siftwing_base siftwing_term_frequency PRIVATE siftwing_utf8cpp)

# 原始Posting只依赖强文档身份/受检整数，不引入词典、分词或权重算法依赖。
add_library(siftwing_posting STATIC src/index/posting.cc)
target_link_libraries(siftwing_posting PUBLIC siftwing_base)

add_library(siftwing_inverted_index STATIC src/index/inverted_index.cc)
target_link_libraries(siftwing_inverted_index PUBLIC siftwing_lexicon siftwing_posting)

add_library(siftwing_weights STATIC src/index/weighted_index.cc)
target_link_libraries(siftwing_weights PUBLIC siftwing_inverted_index)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    target_compile_options(siftwing_weights PRIVATE -fno-fast-math -ffp-contract=off)
endif()

# 快照清单只校验内存元数据/字节，不耦合文件写入、读取或具体文本管线。
add_library(siftwing_snapshot_manifest STATIC src/snapshot/manifest.cc)
target_link_libraries(siftwing_snapshot_manifest PUBLIC siftwing_base)

# Writer只组合已验证部件；Linux fd层负责完整集合落盘及原子无覆盖发布。
add_library(siftwing_snapshot_writer STATIC src/snapshot/writer.cc)
target_link_libraries(siftwing_snapshot_writer PUBLIC siftwing_snapshot_manifest
    siftwing_document_store siftwing_weights)

# Reader在完整校验后拥有所有部件，不修改已封板codec或保存长期fd。
add_library(siftwing_snapshot_reader STATIC src/snapshot/reader.cc)
target_link_libraries(siftwing_snapshot_reader PUBLIC siftwing_snapshot_manifest
    siftwing_document_store siftwing_weights)

# 建库与查询共享拥有型管线，生产目标只组合文本阶段，不耦合文档读取或具体中文实现。
add_library(siftwing_text_pipeline STATIC src/text/text_pipeline.cc)
target_link_libraries(siftwing_text_pipeline PUBLIC siftwing_text_normalization siftwing_term_frequency)

# Query只借用共享管线生成拥有TF，不依赖快照、候选合并或排序。
add_library(siftwing_query STATIC src/query/query.cc)
target_link_libraries(siftwing_query PUBLIC siftwing_text_pipeline)

# 候选只组合有效Query与原始倒排，不依赖权重或快照IO。
add_library(siftwing_candidates STATIC src/query/candidates.cc)
target_link_libraries(siftwing_candidates PUBLIC siftwing_query siftwing_inverted_index)

# 排序只组合候选与已有单位权重；查询数值沿相同的严格浮点策略。
add_library(siftwing_ranking STATIC src/query/ranking.cc)
target_link_libraries(siftwing_ranking PUBLIC siftwing_candidates siftwing_weights)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    target_compile_options(siftwing_ranking PRIVATE -fno-fast-math -ffp-contract=off)
endif()

# top-k只缩小完整排序的拥有结果，保留评分与完整工作准入。
add_library(siftwing_top_k STATIC src/query/top_k.cc)
target_link_libraries(siftwing_top_k PUBLIC siftwing_ranking)

# 摘要只消费Query与原文；严格Unicode解码复用已固定的头文件依赖。
add_library(siftwing_summary STATIC src/query/summary.cc)
target_link_libraries(siftwing_summary PUBLIC siftwing_query PRIVATE siftwing_utf8cpp)

# 高亮匹配原文后才转义；只依赖Query，不改变摘要或快照合同。
add_library(siftwing_highlight STATIC src/query/highlight.cc)
target_link_libraries(siftwing_highlight PUBLIC siftwing_query PRIVATE siftwing_utf8cpp)

# 推荐只消费独立词典，不依赖搜索Query、评分或快照IO。
add_library(siftwing_recommendation STATIC src/query/recommendation.cc)
target_link_libraries(siftwing_recommendation PUBLIC siftwing_lexicon PRIVATE siftwing_utf8cpp)

# 指纹只消费完整文档TF，不依赖读取器或具体分词资源，不决定去重保留集合。
add_library(siftwing_simhash STATIC src/document/simhash.cc)
target_link_libraries(siftwing_simhash PUBLIC siftwing_term_frequency PRIVATE siftwing_utf8cpp)

add_library(siftwing_duplicate_decision STATIC src/document/duplicate_decision.cc)
target_link_libraries(siftwing_duplicate_decision PUBLIC siftwing_document siftwing_simhash)

add_library(siftwing_txt_reader STATIC src/document/txt_reader.cc)
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

add_library(siftwing_html_text STATIC src/document/html_text.cc)
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
add_library(siftwing_rss_reader STATIC src/document/rss_reader.cc)
target_link_libraries(siftwing_rss_reader PUBLIC siftwing_document_reader
    PRIVATE siftwing_tinyxml2 siftwing_utf8cpp siftwing_html_text)

add_library(siftwing_bootstrap STATIC src/bootstrap/byte_probe.cc)
target_include_directories(siftwing_bootstrap PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(siftwing_bootstrap PUBLIC siftwing_build_options)

add_executable(siftwing_build_probe apps/build_probe/main.cc)
target_link_libraries(siftwing_build_probe PRIVATE siftwing_bootstrap)

# 正式离线入口：两支独立输入/资源，最后复用完整快照发布，不新增持久化格式。
add_executable(siftwing_indexer apps/indexer/main.cc)
set_target_properties(siftwing_indexer PROPERTIES OUTPUT_NAME siftwing-indexer)
target_link_libraries(siftwing_indexer PRIVATE siftwing_txt_reader siftwing_rss_reader
    siftwing_text_pipeline siftwing_cppjieba_tokenizer siftwing_duplicate_decision
    siftwing_snapshot_writer siftwing_tinyxml2)

# 本地查询入口只消费完整只读快照；显示/推荐沿用各自核心，JSON限于固定结果schema。
add_executable(siftwing_cli apps/cli/main.cc)
set_target_properties(siftwing_cli PROPERTIES OUTPUT_NAME siftwing-cli)
target_link_libraries(siftwing_cli PRIVATE siftwing_snapshot_reader siftwing_top_k
    siftwing_summary siftwing_highlight siftwing_recommendation siftwing_cppjieba_tokenizer siftwing_utf8cpp)

include(CTest)
if(BUILD_TESTING)
    add_executable(siftwing_frame_decoder_test tests/unit/frame_decoder_test.cc)
    target_link_libraries(siftwing_frame_decoder_test PRIVATE siftwing_frame_decoder)
    add_test(NAME net.frame_decoder COMMAND siftwing_frame_decoder_test)
    set_tests_properties(net.frame_decoder PROPERTIES TIMEOUT 30)
    add_executable(siftwing_buffer_test tests/unit/buffer_test.cc)
    target_link_libraries(siftwing_buffer_test PRIVATE siftwing_buffer)
    add_test(NAME net.buffer COMMAND siftwing_buffer_test)
    set_tests_properties(net.buffer PROPERTIES TIMEOUT 30)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_executable(siftwing_byte_probe_test tests/unit/byte_probe_test.cc)
    target_link_libraries(siftwing_byte_probe_test PRIVATE siftwing_bootstrap)
    add_test(NAME bootstrap.byte_probe COMMAND siftwing_byte_probe_test)
    add_test(NAME bootstrap.cli
        COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:siftwing_build_probe>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/build_probe_cli.cmake"
    )
    set_tests_properties(bootstrap.byte_probe bootstrap.cli PROPERTIES TIMEOUT 10)
    add_test(NAME app.indexer_integration COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/indexer_test.py"
        --indexer "$<TARGET_FILE:siftwing_indexer>" --reader "$<TARGET_FILE:siftwing_reader_probe>" --suite integration)
    add_test(NAME app.indexer_failures COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/indexer_test.py"
        --indexer "$<TARGET_FILE:siftwing_indexer>" --reader "$<TARGET_FILE:siftwing_reader_probe>" --suite failures)
    set_tests_properties(app.indexer_integration app.indexer_failures PROPERTIES TIMEOUT 120)
    add_test(NAME app.cli_integration COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_test.py"
        --cli "$<TARGET_FILE:siftwing_cli>" --indexer "$<TARGET_FILE:siftwing_indexer>"
        --writer "$<TARGET_FILE:siftwing_writer_probe>" --suite integration)
    add_test(NAME app.cli_failures COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_test.py"
        --cli "$<TARGET_FILE:siftwing_cli>" --indexer "$<TARGET_FILE:siftwing_indexer>"
        --writer "$<TARGET_FILE:siftwing_writer_probe>" --suite failures)
    set_tests_properties(app.cli_integration app.cli_failures PROPERTIES TIMEOUT 120)
    # 固定黄金走正式应用；探针仅观察同进程生命周期，不能以进程退出替代资源交接证明。
    add_executable(siftwing_v1_lifecycle_probe tests/integration/v1_lifecycle_probe.cc)
    target_link_libraries(siftwing_v1_lifecycle_probe PRIVATE siftwing_snapshot_reader siftwing_top_k
        siftwing_summary siftwing_highlight siftwing_recommendation siftwing_cppjieba_tokenizer)
    foreach(suite IN ITEMS golden rejection lifecycle)
        add_test(NAME v1.${suite} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/v1_golden_test.py"
            --indexer "$<TARGET_FILE:siftwing_indexer>" --cli "$<TARGET_FILE:siftwing_cli>"
            --probe "$<TARGET_FILE:siftwing_v1_lifecycle_probe>"
            --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden"
            --data "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v1" --suite "${suite}")
        set_tests_properties(v1.${suite} PROPERTIES TIMEOUT 120)
    endforeach()
    add_test(NAME fixtures.integrity
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/golden_fixtures_test.py"
            --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden"
            --project-license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    )
    set_tests_properties(fixtures.integrity PROPERTIES TIMEOUT 15)

    add_executable(siftwing_result_test tests/unit/result_test.cc)
    target_link_libraries(siftwing_result_test PRIVATE siftwing_base)
    add_test(NAME base.result COMMAND siftwing_result_test)
    set_tests_properties(base.result PROPERTIES TIMEOUT 10)
    add_executable(siftwing_checked_test tests/unit/checked_test.cc)
    target_link_libraries(siftwing_checked_test PRIVATE siftwing_base)
    add_test(NAME base.checked COMMAND siftwing_checked_test)
    set_tests_properties(base.checked PROPERTIES TIMEOUT 30)
    add_executable(siftwing_id_test tests/unit/id_test.cc)
    target_link_libraries(siftwing_id_test PRIVATE siftwing_base)
    add_test(NAME base.id COMMAND siftwing_id_test)
    set_tests_properties(base.id PROPERTIES TIMEOUT 10)
    add_executable(siftwing_document_record_test tests/unit/document_record_test.cc)
    target_link_libraries(siftwing_document_record_test PRIVATE siftwing_document)
    add_test(NAME document.record COMMAND siftwing_document_record_test)

    add_executable(siftwing_record_format_test tests/unit/record_format_test.cc)
    target_link_libraries(siftwing_record_format_test PRIVATE siftwing_record_format)
    add_test(NAME document.record_format COMMAND siftwing_record_format_test)
    add_executable(siftwing_record_format_probe tests/integration/record_format_probe.cc)
    target_link_libraries(siftwing_record_format_probe PRIVATE siftwing_record_format)
    add_test(NAME document.record_format_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/record_format_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_record_format_probe>")
    set_tests_properties(document.record_format document.record_format_reference PROPERTIES TIMEOUT 30)
    add_executable(siftwing_document_store_test tests/unit/document_store_test.cc)
    target_link_libraries(siftwing_document_store_test PRIVATE siftwing_document_store)
    target_link_options(siftwing_document_store_test PRIVATE "-Wl,--wrap=pread")
    add_test(NAME document.store COMMAND siftwing_document_store_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_document_store_probe tests/integration/document_store_probe.cc)
    target_link_libraries(siftwing_document_store_probe PRIVATE siftwing_document_store)
    add_test(NAME document.store_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/document_store_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_document_store_probe>")
    set_tests_properties(document.store document.store_reference PROPERTIES TIMEOUT 30)
    set_tests_properties(document.record PROPERTIES TIMEOUT 10)
    add_executable(siftwing_document_reader_test tests/unit/document_reader_test.cc)
    target_link_libraries(siftwing_document_reader_test PRIVATE siftwing_document_reader)
    target_compile_definitions(siftwing_document_reader_test PRIVATE
        SIFTWING_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden")
    add_test(NAME document.reader COMMAND siftwing_document_reader_test)
    set_tests_properties(document.reader PROPERTIES TIMEOUT 10)
    add_executable(siftwing_txt_reader_test tests/unit/txt_reader_test.cc)
    target_link_libraries(siftwing_txt_reader_test PRIVATE siftwing_txt_reader)
    add_test(NAME document.txt_reader COMMAND siftwing_txt_reader_test)
    set_tests_properties(document.txt_reader PROPERTIES TIMEOUT 15)
    # 显式文件接入验收探针；公开CTest只生成原创输入，不依赖私有语料。
    add_executable(siftwing_txt_corpus_probe tests/integration/txt_corpus_probe.cc)
    target_link_libraries(siftwing_txt_corpus_probe PRIVATE siftwing_txt_reader)
    add_test(NAME document.txt_file_ingestion
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/txt_file_ingestion_test.py"
            --probe "$<TARGET_FILE:siftwing_txt_corpus_probe>")
    set_tests_properties(document.txt_file_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_html_text_test tests/unit/html_text_test.cc)
    target_link_libraries(siftwing_html_text_test PRIVATE siftwing_html_text)
    add_test(NAME document.html_text COMMAND siftwing_html_text_test)
    set_tests_properties(document.html_text PROPERTIES TIMEOUT 20)
    add_executable(siftwing_html_fragment_probe tests/integration/html_fragment_probe.cc)
    target_link_libraries(siftwing_html_fragment_probe PRIVATE siftwing_html_text)
    add_test(NAME document.html_fragment_integration
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/html_fragment_integration_test.py"
            --probe "$<TARGET_FILE:siftwing_html_fragment_probe>"
            --rss "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden/rss/original.xml")
    set_tests_properties(document.html_fragment_integration PROPERTIES TIMEOUT 30)
    add_executable(siftwing_rss_reader_test tests/unit/rss_reader_test.cc)
    target_link_libraries(siftwing_rss_reader_test PRIVATE siftwing_rss_reader)
    add_test(NAME document.rss_reader COMMAND siftwing_rss_reader_test)
    set_tests_properties(document.rss_reader PROPERTIES TIMEOUT 30)
    add_executable(siftwing_rss_corpus_probe tests/integration/rss_corpus_probe.cc)
    target_link_libraries(siftwing_rss_corpus_probe PRIVATE siftwing_rss_reader)
    add_test(NAME document.rss_ingestion
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/rss_ingestion_test.py"
            --probe "$<TARGET_FILE:siftwing_rss_corpus_probe>"
            --rss "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden/rss/original.xml")
    set_tests_properties(document.rss_ingestion PROPERTIES TIMEOUT 30)
    add_executable(siftwing_normalize_test tests/unit/normalize_test.cc)
    target_link_libraries(siftwing_normalize_test PRIVATE siftwing_text_normalization)
    add_test(NAME text.normalization COMMAND siftwing_normalize_test)
    set_tests_properties(text.normalization PROPERTIES TIMEOUT 15)
    add_executable(siftwing_normalize_probe tests/integration/normalize_probe.cc)
    target_link_libraries(siftwing_normalize_probe PRIVATE siftwing_text_normalization)
    add_test(NAME text.normalization_reference
        COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/normalize_reference_test.py"
            --probe "$<TARGET_FILE:siftwing_normalize_probe>")
    set_tests_properties(text.normalization_reference PROPERTIES TIMEOUT 60)
    add_executable(siftwing_normalization_ingestion_test tests/integration/text_normalization_ingestion_test.cc)
    target_link_libraries(siftwing_normalization_ingestion_test PRIVATE
        siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.normalization_ingestion COMMAND siftwing_normalization_ingestion_test)
    set_tests_properties(text.normalization_ingestion PROPERTIES TIMEOUT 15)
    add_executable(siftwing_english_tokenizer_test tests/unit/english_tokenizer_test.cc)
    target_link_libraries(siftwing_english_tokenizer_test PRIVATE siftwing_tokenizer)
    add_test(NAME text.english_tokenizer COMMAND siftwing_english_tokenizer_test)
    set_tests_properties(text.english_tokenizer PROPERTIES TIMEOUT 15)
    add_executable(siftwing_english_tokenizer_probe tests/integration/english_tokenizer_probe.cc)
    target_link_libraries(siftwing_english_tokenizer_probe PRIVATE siftwing_tokenizer)
    add_test(NAME text.english_tokenizer_reference
        COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/english_tokenizer_reference_test.py"
            --probe "$<TARGET_FILE:siftwing_english_tokenizer_probe>")
    set_tests_properties(text.english_tokenizer_reference PROPERTIES TIMEOUT 60)
    add_executable(siftwing_english_tokenizer_ingestion_test tests/integration/english_tokenizer_ingestion_test.cc)
    target_link_libraries(siftwing_english_tokenizer_ingestion_test PRIVATE
        siftwing_tokenizer siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.english_tokenizer_ingestion COMMAND siftwing_english_tokenizer_ingestion_test)
    set_tests_properties(text.english_tokenizer_ingestion PROPERTIES TIMEOUT 15)
    add_executable(siftwing_cppjieba_tokenizer_test tests/unit/cppjieba_tokenizer_test.cc)
    target_link_libraries(siftwing_cppjieba_tokenizer_test PRIVATE siftwing_cppjieba_tokenizer)
    add_test(NAME text.cppjieba_tokenizer COMMAND siftwing_cppjieba_tokenizer_test)
    add_executable(siftwing_cppjieba_tokenizer_probe tests/integration/cppjieba_tokenizer_probe.cc)
    target_link_libraries(siftwing_cppjieba_tokenizer_probe PRIVATE siftwing_cppjieba_tokenizer)
    add_test(NAME text.cppjieba_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cppjieba_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_cppjieba_tokenizer_probe>")
    add_executable(siftwing_cppjieba_ingestion_test tests/integration/cppjieba_tokenizer_ingestion_test.cc)
    target_link_libraries(siftwing_cppjieba_ingestion_test PRIVATE siftwing_cppjieba_tokenizer
        siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.cppjieba_ingestion COMMAND siftwing_cppjieba_ingestion_test)
    set_tests_properties(text.cppjieba_tokenizer text.cppjieba_reference text.cppjieba_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_term_frequency_test tests/unit/term_frequency_test.cc)
    target_link_libraries(siftwing_term_frequency_test PRIVATE siftwing_term_frequency)
    add_test(NAME text.term_frequency COMMAND siftwing_term_frequency_test)
    add_executable(siftwing_term_frequency_probe tests/integration/term_frequency_probe.cc)
    target_link_libraries(siftwing_term_frequency_probe PRIVATE siftwing_term_frequency)
    add_test(NAME text.term_frequency_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/term_frequency_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_term_frequency_probe>")
    add_executable(siftwing_term_frequency_ingestion_test tests/integration/term_frequency_ingestion_test.cc)
    target_link_libraries(siftwing_term_frequency_ingestion_test PRIVATE siftwing_term_frequency
        siftwing_cppjieba_tokenizer siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.term_frequency_ingestion COMMAND siftwing_term_frequency_ingestion_test)
    set_tests_properties(text.term_frequency text.term_frequency_reference text.term_frequency_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_lexicon_test tests/unit/lexicon_test.cc)
    target_link_libraries(siftwing_lexicon_test PRIVATE siftwing_lexicon)
    add_test(NAME index.lexicon COMMAND siftwing_lexicon_test)
    add_executable(siftwing_lexicon_probe tests/integration/lexicon_probe.cc)
    target_link_libraries(siftwing_lexicon_probe PRIVATE siftwing_lexicon)
    add_test(NAME index.lexicon_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lexicon_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_lexicon_probe>")
    add_executable(siftwing_lexicon_ingestion_test tests/integration/lexicon_ingestion_test.cc)
    target_link_libraries(siftwing_lexicon_ingestion_test PRIVATE siftwing_lexicon
        siftwing_text_pipeline siftwing_duplicate_decision)
    add_test(NAME index.lexicon_ingestion COMMAND siftwing_lexicon_ingestion_test)
    set_tests_properties(index.lexicon index.lexicon_reference index.lexicon_ingestion PROPERTIES TIMEOUT 30)
    add_executable(siftwing_posting_test tests/unit/posting_test.cc)
    target_link_libraries(siftwing_posting_test PRIVATE siftwing_posting)
    add_test(NAME index.posting COMMAND siftwing_posting_test)
    add_executable(siftwing_posting_ingestion_test tests/integration/posting_ingestion_test.cc)
    target_link_libraries(siftwing_posting_ingestion_test PRIVATE siftwing_posting siftwing_term_frequency)
    add_test(NAME index.posting_ingestion COMMAND siftwing_posting_ingestion_test)
    set_tests_properties(index.posting index.posting_ingestion PROPERTIES TIMEOUT 15)
    add_executable(siftwing_inverted_index_test tests/unit/inverted_index_test.cc)
    target_link_libraries(siftwing_inverted_index_test PRIVATE siftwing_inverted_index)
    add_test(NAME index.inverted_index COMMAND siftwing_inverted_index_test)
    add_executable(siftwing_inverted_index_probe tests/integration/inverted_index_probe.cc)
    target_link_libraries(siftwing_inverted_index_probe PRIVATE siftwing_inverted_index)
    add_test(NAME index.inverted_index_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/inverted_index_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_inverted_index_probe>")
    add_executable(siftwing_inverted_index_ingestion_test tests/integration/inverted_index_ingestion_test.cc)
    target_link_libraries(siftwing_inverted_index_ingestion_test PRIVATE siftwing_inverted_index
        siftwing_text_pipeline siftwing_tokenizer siftwing_duplicate_decision)
    add_test(NAME index.inverted_index_ingestion COMMAND siftwing_inverted_index_ingestion_test)
    add_test(NAME index.inverted_index_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/inverted-index-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/inverted_index_compile_contract.cmake")
    set_tests_properties(index.inverted_index index.inverted_index_reference
        index.inverted_index_ingestion index.inverted_index_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_weighted_index_test tests/unit/weighted_index_test.cc)
    target_link_libraries(siftwing_weighted_index_test PRIVATE siftwing_weights)
    add_test(NAME index.weights COMMAND siftwing_weighted_index_test)
    add_executable(siftwing_weighted_index_probe tests/integration/weighted_index_probe.cc)
    target_link_libraries(siftwing_weighted_index_probe PRIVATE siftwing_weights)
    add_test(NAME index.weights_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/weighted_index_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_weighted_index_probe>")
    add_executable(siftwing_weighted_index_ingestion_test tests/integration/weighted_index_ingestion_test.cc)
    target_link_libraries(siftwing_weighted_index_ingestion_test PRIVATE siftwing_weights
        siftwing_text_pipeline siftwing_tokenizer siftwing_duplicate_decision)
    add_test(NAME index.weights_ingestion COMMAND siftwing_weighted_index_ingestion_test)
    add_test(NAME index.weights_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/weights-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/weighted_index_compile_contract.cmake")
    set_tests_properties(index.weights index.weights_reference index.weights_ingestion
        index.weights_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_manifest_test tests/unit/manifest_test.cc)
    target_link_libraries(siftwing_manifest_test PRIVATE siftwing_snapshot_manifest)
    add_test(NAME snapshot.manifest COMMAND siftwing_manifest_test)
    add_executable(siftwing_manifest_probe tests/integration/manifest_probe.cc)
    target_link_libraries(siftwing_manifest_probe PRIVATE siftwing_snapshot_manifest)
    add_test(NAME snapshot.manifest_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/manifest_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_manifest_probe>")
    add_executable(siftwing_manifest_ingestion_test tests/integration/manifest_ingestion_test.cc)
    target_link_libraries(siftwing_manifest_ingestion_test PRIVATE siftwing_snapshot_manifest
        siftwing_weights siftwing_document_store siftwing_text_pipeline siftwing_tokenizer)
    add_test(NAME snapshot.manifest_ingestion COMMAND siftwing_manifest_ingestion_test)
    add_test(NAME snapshot.manifest_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/manifest-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/manifest_compile_contract.cmake")
    set_tests_properties(snapshot.manifest snapshot.manifest_reference snapshot.manifest_ingestion
        snapshot.manifest_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_writer_test tests/unit/writer_test.cc)
    target_link_libraries(siftwing_writer_test PRIVATE siftwing_snapshot_writer)
    foreach(operation openat write pread getrandom mkdirat fsync close fstat fdopendir renameat2)
        target_link_options(siftwing_writer_test PRIVATE "-Wl,--wrap=${operation}")
    endforeach()
    add_test(NAME snapshot.writer COMMAND siftwing_writer_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_writer_probe tests/integration/writer_probe.cc)
    target_link_libraries(siftwing_writer_probe PRIVATE siftwing_snapshot_writer)
    add_test(NAME snapshot.writer_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/writer_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_writer_probe>" --parent "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_writer_race_test tests/integration/writer_race_test.cc)
    target_link_libraries(siftwing_writer_race_test PRIVATE siftwing_snapshot_writer)
    target_link_options(siftwing_writer_race_test PRIVATE "-Wl,--wrap=renameat2")
    add_test(NAME snapshot.writer_race COMMAND siftwing_writer_race_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_writer_ingestion_test tests/integration/writer_ingestion_test.cc)
    target_link_libraries(siftwing_writer_ingestion_test PRIVATE siftwing_snapshot_writer
        siftwing_text_pipeline siftwing_tokenizer)
    add_test(NAME snapshot.writer_ingestion COMMAND siftwing_writer_ingestion_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME snapshot.writer_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/writer-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/writer_compile_contract.cmake")
    set_tests_properties(snapshot.writer snapshot.writer_reference snapshot.writer_race
        snapshot.writer_ingestion snapshot.writer_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_reader_test tests/unit/reader_test.cc)
    target_link_libraries(siftwing_reader_test PRIVATE siftwing_snapshot_reader siftwing_snapshot_writer)
    foreach(operation openat pread fstat fstatat close fdopendir readdir closedir)
        target_link_options(siftwing_reader_test PRIVATE "-Wl,--wrap=${operation}")
    endforeach()
    add_test(NAME snapshot.reader COMMAND siftwing_reader_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_reader_probe tests/integration/reader_probe.cc)
    target_link_libraries(siftwing_reader_probe PRIVATE siftwing_snapshot_reader)
    add_test(NAME snapshot.reader_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/reader_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_reader_probe>" --parent "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME snapshot.reader_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/reader-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/reader_compile_contract.cmake")
    set_tests_properties(snapshot.reader snapshot.reader_reference snapshot.reader_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_roundtrip_probe tests/integration/roundtrip_probe.cc)
    target_link_libraries(siftwing_roundtrip_probe PRIVATE siftwing_snapshot_reader siftwing_snapshot_writer)
    add_executable(siftwing_roundtrip_pipeline_test tests/integration/roundtrip_pipeline_test.cc)
    target_link_libraries(siftwing_roundtrip_pipeline_test PRIVATE siftwing_snapshot_reader siftwing_snapshot_writer
        siftwing_txt_reader siftwing_rss_reader siftwing_text_pipeline siftwing_tokenizer siftwing_duplicate_decision)
    add_test(NAME snapshot.roundtrip COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/roundtrip_test.py"
        --writer "$<TARGET_FILE:siftwing_writer_probe>" --reader "$<TARGET_FILE:siftwing_reader_probe>"
        --republish "$<TARGET_FILE:siftwing_roundtrip_probe>" --pipeline "$<TARGET_FILE:siftwing_roundtrip_pipeline_test>"
        --parent "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(snapshot.roundtrip PROPERTIES TIMEOUT 120)
    add_executable(siftwing_query_test tests/unit/query_test.cc)
    target_link_libraries(siftwing_query_test PRIVATE siftwing_query siftwing_tokenizer)
    add_test(NAME query.model COMMAND siftwing_query_test)
    add_executable(siftwing_query_probe tests/integration/query_probe.cc)
    target_link_libraries(siftwing_query_probe PRIVATE siftwing_query siftwing_tokenizer)
    add_test(NAME query.reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/query_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_query_probe>")
    add_executable(siftwing_query_ingestion_test tests/integration/query_ingestion_test.cc)
    target_link_libraries(siftwing_query_ingestion_test PRIVATE siftwing_query siftwing_tokenizer
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME query.ingestion COMMAND siftwing_query_ingestion_test)
    add_test(NAME query.compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/query-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/query_compile_contract.cmake")
    set_tests_properties(query.model query.reference query.ingestion query.compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_candidates_test tests/unit/candidates_test.cc)
    target_link_libraries(siftwing_candidates_test PRIVATE siftwing_candidates siftwing_tokenizer)
    add_test(NAME query.candidates COMMAND siftwing_candidates_test)
    add_executable(siftwing_candidates_probe tests/integration/candidates_probe.cc)
    target_link_libraries(siftwing_candidates_probe PRIVATE siftwing_candidates siftwing_tokenizer)
    add_test(NAME query.candidates_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/candidates_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_candidates_probe>")
    add_executable(siftwing_candidates_snapshot_test tests/integration/candidates_snapshot_test.cc)
    target_link_libraries(siftwing_candidates_snapshot_test PRIVATE siftwing_candidates siftwing_tokenizer
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader
        siftwing_snapshot_writer siftwing_snapshot_reader)
    add_test(NAME query.candidates_snapshot COMMAND siftwing_candidates_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.candidates_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/candidates-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/candidates_compile_contract.cmake")
    set_tests_properties(query.candidates query.candidates_reference query.candidates_snapshot
        query.candidates_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_ranking_test tests/unit/ranking_test.cc)
    target_link_libraries(siftwing_ranking_test PRIVATE siftwing_ranking siftwing_tokenizer)
    add_test(NAME query.ranking COMMAND siftwing_ranking_test)
    add_executable(siftwing_ranking_probe tests/integration/ranking_probe.cc)
    target_link_libraries(siftwing_ranking_probe PRIVATE siftwing_ranking siftwing_tokenizer)
    add_test(NAME query.ranking_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/ranking_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_ranking_probe>")
    add_executable(siftwing_ranking_snapshot_test tests/integration/ranking_snapshot_test.cc)
    target_link_libraries(siftwing_ranking_snapshot_test PRIVATE siftwing_ranking siftwing_tokenizer
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader
        siftwing_snapshot_writer siftwing_snapshot_reader)
    add_test(NAME query.ranking_snapshot COMMAND siftwing_ranking_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.ranking_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/ranking-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/ranking_compile_contract.cmake")
    set_tests_properties(query.ranking query.ranking_reference query.ranking_snapshot
        query.ranking_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_summary_test tests/unit/summary_test.cc)
    target_link_libraries(siftwing_summary_test PRIVATE siftwing_summary)
    add_test(NAME query.summary COMMAND siftwing_summary_test)
    add_executable(siftwing_summary_probe tests/integration/summary_probe.cc)
    target_link_libraries(siftwing_summary_probe PRIVATE siftwing_summary)
    add_test(NAME query.summary_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/summary_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_summary_probe>")
    add_executable(siftwing_summary_snapshot_test tests/integration/summary_snapshot_test.cc)
    target_link_libraries(siftwing_summary_snapshot_test PRIVATE siftwing_summary siftwing_top_k
        siftwing_snapshot_writer siftwing_snapshot_reader siftwing_txt_reader siftwing_rss_reader
        siftwing_cppjieba_tokenizer)
    add_test(NAME query.summary_snapshot COMMAND siftwing_summary_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.summary_compile_contract COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/summary-compile-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/summary_compile_contract.cmake")
    set_tests_properties(query.summary query.summary_reference query.summary_snapshot
        query.summary_compile_contract PROPERTIES TIMEOUT 60)

    add_executable(siftwing_recommendation_test tests/unit/recommendation_test.cc)
    target_link_libraries(siftwing_recommendation_test PRIVATE siftwing_recommendation)
    add_test(NAME query.recommendation COMMAND siftwing_recommendation_test)
    add_executable(siftwing_recommendation_probe tests/integration/recommendation_probe.cc)
    target_link_libraries(siftwing_recommendation_probe PRIVATE siftwing_recommendation)
    add_test(NAME query.recommendation_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/recommendation_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_recommendation_probe>")
    add_executable(siftwing_recommendation_snapshot_test tests/integration/recommendation_snapshot_test.cc)
    target_link_libraries(siftwing_recommendation_snapshot_test PRIVATE siftwing_recommendation siftwing_snapshot_reader
        siftwing_snapshot_writer siftwing_text_pipeline siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME query.recommendation_snapshot COMMAND siftwing_recommendation_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.recommendation_compile_contract COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/recommendation-compile-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/recommendation_compile_contract.cmake")
    set_tests_properties(query.recommendation query.recommendation_reference query.recommendation_snapshot
        query.recommendation_compile_contract PROPERTIES TIMEOUT 60)

    add_executable(siftwing_highlight_test tests/unit/highlight_test.cc)
    target_link_libraries(siftwing_highlight_test PRIVATE siftwing_highlight)
    add_test(NAME query.highlight COMMAND siftwing_highlight_test)
    add_executable(siftwing_highlight_probe tests/integration/highlight_probe.cc)
    target_link_libraries(siftwing_highlight_probe PRIVATE siftwing_highlight)
    add_test(NAME query.highlight_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/highlight_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_highlight_probe>")
    add_executable(siftwing_highlight_snapshot_test tests/integration/highlight_snapshot_test.cc)
    target_link_libraries(siftwing_highlight_snapshot_test PRIVATE siftwing_highlight siftwing_summary siftwing_top_k
        siftwing_snapshot_writer siftwing_snapshot_reader siftwing_txt_reader siftwing_rss_reader
        siftwing_cppjieba_tokenizer)
    add_test(NAME query.highlight_snapshot COMMAND siftwing_highlight_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.highlight_compile_contract COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/highlight-compile-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/highlight_compile_contract.cmake")
    set_tests_properties(query.highlight query.highlight_reference query.highlight_snapshot
        query.highlight_compile_contract PROPERTIES TIMEOUT 60)

    add_executable(siftwing_top_k_test tests/unit/top_k_test.cc)
    target_link_libraries(siftwing_top_k_test PRIVATE siftwing_top_k siftwing_tokenizer)
    add_test(NAME query.top_k COMMAND siftwing_top_k_test)
    add_executable(siftwing_top_k_probe tests/integration/top_k_probe.cc)
    target_link_libraries(siftwing_top_k_probe PRIVATE siftwing_top_k siftwing_tokenizer)
    add_test(NAME query.top_k_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/top_k_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_top_k_probe>")
    add_executable(siftwing_top_k_snapshot_test tests/integration/top_k_snapshot_test.cc)
    target_link_libraries(siftwing_top_k_snapshot_test PRIVATE siftwing_top_k siftwing_tokenizer
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader
        siftwing_snapshot_writer siftwing_snapshot_reader)
    add_test(NAME query.top_k_snapshot COMMAND siftwing_top_k_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.top_k_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/top-k-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/top_k_compile_contract.cmake")
    set_tests_properties(query.top_k query.top_k_reference query.top_k_snapshot
        query.top_k_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_text_pipeline_test tests/unit/text_pipeline_test.cc)
    target_link_libraries(siftwing_text_pipeline_test PRIVATE siftwing_text_pipeline)
    add_test(NAME text.pipeline COMMAND siftwing_text_pipeline_test)
    add_executable(siftwing_text_pipeline_ingestion_test tests/integration/text_pipeline_ingestion_test.cc)
    target_link_libraries(siftwing_text_pipeline_ingestion_test PRIVATE siftwing_text_pipeline
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.pipeline_ingestion COMMAND siftwing_text_pipeline_ingestion_test)
    set_tests_properties(text.pipeline text.pipeline_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_text_pipeline_probe tests/integration/text_pipeline_probe.cc)
    target_link_libraries(siftwing_text_pipeline_probe PRIVATE siftwing_text_pipeline siftwing_cppjieba_tokenizer)
    add_test(NAME text.pipeline_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/text_pipeline_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_text_pipeline_probe>")
    set_tests_properties(text.pipeline_reference PROPERTIES TIMEOUT 120)
    add_executable(siftwing_simhash_test tests/unit/simhash_test.cc)
    target_link_libraries(siftwing_simhash_test PRIVATE siftwing_simhash)
    add_test(NAME document.simhash COMMAND siftwing_simhash_test)
    add_executable(siftwing_simhash_ingestion_test tests/integration/simhash_ingestion_test.cc)
    target_link_libraries(siftwing_simhash_ingestion_test PRIVATE siftwing_simhash
        siftwing_text_pipeline siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME document.simhash_ingestion COMMAND siftwing_simhash_ingestion_test)
    add_executable(siftwing_simhash_probe tests/integration/simhash_probe.cc)
    target_link_libraries(siftwing_simhash_probe PRIVATE siftwing_simhash)
    add_test(NAME document.simhash_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/simhash_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_simhash_probe>")
    set_tests_properties(document.simhash document.simhash_ingestion document.simhash_reference PROPERTIES TIMEOUT 30)
    add_executable(siftwing_duplicate_decision_test tests/unit/duplicate_decision_test.cc)
    target_link_libraries(siftwing_duplicate_decision_test PRIVATE siftwing_duplicate_decision)
    add_test(NAME document.duplicate_decision COMMAND siftwing_duplicate_decision_test)
    add_executable(siftwing_duplicate_decision_ingestion_test tests/integration/duplicate_decision_ingestion_test.cc)
    target_link_libraries(siftwing_duplicate_decision_ingestion_test PRIVATE siftwing_duplicate_decision
        siftwing_text_pipeline siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME document.duplicate_ingestion COMMAND siftwing_duplicate_decision_ingestion_test)
    add_executable(siftwing_duplicate_decision_probe tests/integration/duplicate_decision_probe.cc)
    target_link_libraries(siftwing_duplicate_decision_probe PRIVATE siftwing_duplicate_decision)
    add_test(NAME document.duplicate_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/duplicate_decision_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_duplicate_decision_probe>")
    set_tests_properties(document.duplicate_decision document.duplicate_ingestion document.duplicate_reference PROPERTIES TIMEOUT 30)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        add_test(NAME index.posting_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/posting-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/posting_compile_contract.cmake")
        set_tests_properties(index.posting_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME index.lexicon_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/lexicon-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lexicon_compile_contract.cmake")
        set_tests_properties(index.lexicon_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.store_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/document-store-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/document_store_compile_contract.cmake")
        set_tests_properties(document.store_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.duplicate_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/duplicate-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/duplicate_decision_compile_contract.cmake")
        set_tests_properties(document.duplicate_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.simhash_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/simhash-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/simhash_compile_contract.cmake")
        set_tests_properties(document.simhash_compile_contract PROPERTIES TIMEOUT 30)
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
