find_package(Threads REQUIRED)

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

# 固定 JSON 头文件归档；不运行上游安装/测试，公共 codec 接口不暴露第三方类型。
FetchContent_Declare(nlohmann_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
    URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
    SOURCE_SUBDIR siftwing-header-only
)
FetchContent_MakeAvailable(nlohmann_json)
add_library(siftwing_json INTERFACE)
target_include_directories(siftwing_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}/include")
# 固定spdlog及其bundled fmt；同步header-only，不注册全局logger或启用上游工程。
FetchContent_Declare(spdlog
    URL https://codeload.github.com/gabime/spdlog/tar.gz/refs/tags/v1.15.3
    URL_HASH SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67
    SOURCE_SUBDIR siftwing-header-only
)
FetchContent_MakeAvailable(spdlog)
add_library(siftwing_spdlog INTERFACE)
target_include_directories(siftwing_spdlog SYSTEM INTERFACE "${spdlog_SOURCE_DIR}/include")

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
