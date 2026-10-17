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
option(SIFTWING_BUILD_CODEC_FUZZER "Build the bounded Clang libFuzzer codec target" OFF)
option(SIFTWING_BUILD_BENCHMARKS "构建独立TCP性能发生器" OFF)

# 从源码根目录配置；生成物由-B指定，Debug/Release使用不同目录。
include(cmake/BuildOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Libraries.cmake)
include(cmake/Applications.cmake)
include(CTest)
include(cmake/Tests.cmake)
