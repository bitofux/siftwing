# 按学习范围构建

从仓库根目录运行 CMake，生成物放在 `build/` 的独立配置目录。
项目业务实现只有两个版本静态库，构建探针、第三方库和测试辅助库仍独立保留。

```text
siftwing_v1（libsiftwing_v1.a）
  ├─ siftwing-indexer
  ├─ siftwing-v1-cli
  └─ siftwing_full（libsiftwing_full.a；PUBLIC依赖V1）
       ├─ siftwing-cli
       ├─ siftwing-server
       └─ siftwing-protocol-probe
```

V1 归档包含文档、文本、索引、快照读写和本地查询的 27 个实现文件。
full 归档只包含协议、网络、并发、缓存、配置、日志、指标、快照管理/加载和在线服务的
32 个实现文件。两个库共享当前源码树中的 V1 实现，不还原历史提交中的 V1 代码。

## 先学习 V1

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target siftwing_v1_apps --parallel 2
build/debug/siftwing-indexer --help
build/debug/siftwing-v1-cli --help
```

`siftwing_v1_apps` 只构建建库程序、本地查询程序及其依赖，不编译 full。
配置阶段仍声明完整工程及固定版本第三方依赖；下载、离线覆盖和许可证见
[依赖说明](DEPENDENCIES.md)。纯 V1 CLI 支持本地 SEARCH/SUGGEST，不提供 `--network`。
参数与[现有 CLI 的本地模式](CLI.md)相同；[V1 回放](V1_DEBUGGING.md)中的本地 CLI
命令可以将程序路径换成 `siftwing-v1-cli`。

## 再串起 V2/V3

```sh
cmake --build build/debug --target siftwing_full_apps --parallel 2
build/debug/siftwing-cli --help
build/debug/siftwing-server --help
```

full 的构建依赖会先生成 V1。V1 的对象文件不重新编译，也不复制进 full 归档。
使用 `target_link_libraries(my_program PRIVATE siftwing_full)` 时，CMake 自动安排
full、V1 和所需第三方库的链接。单独复制 `libsiftwing_full.a` 不包含这些依赖关系，
不能把它当作一个包含所有依赖的独立归档分发。

`siftwing_full_apps` 构建所有版本程序；`siftwing-cli` 保留本地与网络双模式。
服务端和协议探针的现有命令行合同保持，真实网络闭环见[演示](DEMO.md)。

## 添加自己的学习程序

在根配置中加入自己的目标，例如（示例源文件由学习者自行创建）：

```cmake
add_executable(my_v1_lab learning/v1_main.cc)
target_link_libraries(my_v1_lab PRIVATE siftwing_v1)

add_executable(my_online_lab learning/online_main.cc)
target_link_libraries(my_online_lab PRIVATE siftwing_full)
```

每个程序有自己的 `main()`，库只收纳可复用实现。业务库传递公共头文件路径、
项目警告和可选消毒器配置。若学习程序直接使用第三方头文件，还需显式链接相应
第三方目标，例如 `siftwing_json`；业务库的私有头文件依赖不成为调用方的编译接口。

## 完整回归与配置位置

程序聚合目标不会构建全部测试。运行完整 CTest 前先完成默认构建：

```sh
cmake --build build/debug --parallel 2
ctest --test-dir build/debug --output-on-failure --no-tests=error
```

Release 使用独立的 `build/release`，配置命令增加 `-DCMAKE_BUILD_TYPE=Release`。
完整双配置、可选基准和消毒器命令见[测试指南](TESTING.md)。
C++17、关闭扩展、五类警告、权重/排序的严格浮点策略及故障注入链接选项继续保留。

| 配置文件 | 职责 |
|---|---|
| `CMakeLists.txt` | 标准、选项与统一入口 |
| `cmake/BuildOptions.cmake` | 警告、消毒器、公共头文件路径 |
| `cmake/Dependencies.cmake` | 固定依赖、归档哈希、第三方编译规则 |
| `cmake/Libraries.cmake` | 两个业务库的源码清单、依赖与组件别名 |
| `cmake/Applications.cmake` | 程序入口与两个学习聚合目标 |
| `cmake/Tests.cmake` | CTest、故障注入、可选基准与模糊测试工具 |

组件参考中的 `siftwing_query`、`siftwing_connection` 等旧名称保留为对应版本库的
CMake `ALIAS`，不再生成同名小归档。它们可用于 `target_link_libraries`，不能作为
`cmake --build --target` 的独立构建目标；单独构建库请使用 `siftwing_v1` 或 `siftwing_full`。
组件之间的逻辑依赖仍帮助理解源码，实际归档和链接关系以本页及当前 CMake 为准。
