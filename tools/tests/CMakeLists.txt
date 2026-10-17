cmake_minimum_required(VERSION 3.20)
project(siftwing_html_rss_tests LANGUAGES NONE)

# 独立离线工具测试：显式选择装有转换依赖的Python，不影响C++服务依赖。
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_test(NAME tools.html_to_rss
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/test_html_to_rss.py")
set_tests_properties(tools.html_to_rss PROPERTIES TIMEOUT 90)
