# 项目自身的警告、消毒器和公共头文件入口。
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
