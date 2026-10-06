cmake_minimum_required(VERSION 3.20)
project(siftwing LANGUAGES CXX)

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
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        add_test(NAME base.result_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/result-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/result_compile_contract.cmake")
        set_tests_properties(base.result_compile_contract PROPERTIES TIMEOUT 30)
    endif()
endif()
