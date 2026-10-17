# 每个可执行程序保留自己的main；应用源码不进入版本静态库。
add_executable(siftwing_build_probe apps/build_probe/main.cc)
target_link_libraries(siftwing_build_probe PRIVATE siftwing_bootstrap)

add_executable(siftwing_indexer apps/indexer/main.cc)
set_target_properties(siftwing_indexer PROPERTIES OUTPUT_NAME siftwing-indexer)
target_link_libraries(siftwing_indexer PRIVATE siftwing_v1 siftwing_tinyxml2)

# 同一份本地查询实现分别生成纯V1入口和包含网络模式的完整CLI。
add_executable(siftwing_v1_cli apps/cli/main.cc)
set_target_properties(siftwing_v1_cli PROPERTIES OUTPUT_NAME siftwing-v1-cli)
target_link_libraries(siftwing_v1_cli PRIVATE siftwing_v1 siftwing_utf8cpp)

add_executable(siftwing_cli apps/cli/main.cc apps/cli/network.cc)
set_target_properties(siftwing_cli PROPERTIES OUTPUT_NAME siftwing-cli)
target_compile_definitions(siftwing_cli PRIVATE SIFTWING_CLI_WITH_NETWORK=1)
target_link_libraries(siftwing_cli PRIVATE siftwing_full siftwing_json siftwing_utf8cpp)

add_executable(siftwing_server apps/server/main.cc)
set_target_properties(siftwing_server PROPERTIES OUTPUT_NAME siftwing-server)
target_link_libraries(siftwing_server PRIVATE siftwing_full)

add_executable(siftwing_protocol_probe apps/protocol_probe/main.cc)
set_target_properties(siftwing_protocol_probe PROPERTIES OUTPUT_NAME siftwing-protocol-probe)
target_link_libraries(siftwing_protocol_probe PRIVATE siftwing_full)

# 从根目录按学习范围构建，未选择的版本程序不会被该目标编译。
add_custom_target(siftwing_v1_apps DEPENDS siftwing_indexer siftwing_v1_cli)
add_custom_target(siftwing_full_apps DEPENDS siftwing_v1_apps siftwing_cli siftwing_server
    siftwing_protocol_probe)
