# 构建、测试与验证指南

从仓库根目录执行。公共测试使用原创输入与独立参考，不依赖课程资料、私有语料或本机绝对资源路径。Ubuntu 是活动验证环境；依赖、离线配置和许可见[依赖说明](DEPENDENCIES.md)。

仅学习 V1 或先构建程序时，见[按学习范围构建](BUILDING.md)。`app.v1_cli_*` 复用现有
CLI 正常/拒绝路径参考验证纯 V1 入口；`v1.golden/rejection/lifecycle` 使用纯 V1 CLI，
完整 CLI 的本地及网络行为继续由既有 app/net 用例覆盖。

## 两个完整普通构建

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel 2
ctest --test-dir build/debug --output-on-failure --no-tests=error

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 2
ctest --test-dir build/release --output-on-failure --no-tests=error
```

Debug 与 Release 使用独立目录；行为验证在 Release 下仍有效，不依赖被 NDEBUG 消除的 assert。`--no-tests=error` 防止空测试列表被当作成功；配置、构建和 CTest 都必须分别检查退出码。默认启用 CTest；`-DBUILD_TESTING=OFF` 仅构建产品，不能用来报告测试通过。

M4.03已退役纯旧主从专属测试并迁移有效连接行为；启用负载工具会另注册其合同测试。实际数量以当前源码和配置查询为准，不把文档数字当作执行结果：

```sh
ctest --test-dir build/debug --show-only=json-v1
ctest --test-dir build/debug -N
```

## 按行为定位

以下使用 CTest 原名/前缀，正则须加引号；先构建相应目标。每条命令都带空列表拒绝，避免拼错名称后假成功。

| 目标 | 合同/参考 | 选择入口 |
|---|---|---|
| 原始输入、规范化与文本分析 | [组件参考](COMPONENTS.md)、原创黄金输入 | `-R '^(base\.|document\.|text\.)'` |
| 持久化与加载/发布 | [格式](SNAPSHOT_FORMAT.md)、[发布](SNAPSHOT_PUBLICATION.md) | `-R '^(index\.|snapshot\.)'` |
| 独立数学参考与本地应用 | [V1 验收](V1_TESTS.md)、[CLI](CLI.md) | `-R '^(query\.|app\.|v1\.)'` |
| 协议、并发、资源与服务组合 | [V2 矩阵](V2_TESTING.md) | `-R '^(net\.|concurrency\.|cache\.)'` |
| 配置、日志、指标 | [配置](SERVER_CONFIG.md)、[日志](LOGGING.md)、[指标](METRICS.md) | `-R '^(config\.|logging\.|metrics\.)'` |
| 故障、持续运行与关闭竞态 | [可靠性指南](V3_RELIABILITY.md) | `-R '^v3\.'` |

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^snapshot\.'
```

编译合同会验证公共头、自包含性和受禁用法；Python 独立参考覆盖字节布局、整数/数学和进程交互。某个分组通过不代替完整回归，测试名包含某种机制也不代表覆盖全部调度。

## 消毒器的独立范围

ASan/UBSan 与 TSan 分别使用全新 Debug 目录和 Clang 18。不要复用普通目录或把两类插桩叠加。完整参数、明确目标与精确13项选择式直接以[公开 workflow](../.github/workflows/ci.yml)和[CI 说明](CI.md)为准；复制 sanitizer job 的安装后步骤，在仓库根执行并分别更换其 `build/ci` 路径。

ASan/UBSan 配置包含 `-DSIFTWING_ENABLE_SANITIZERS=ON`；TSan 给 C/C++ 编译及可执行链接分别设置 `-fsanitize=thread`，编译加 `-fno-omit-frame-pointer`。前者设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，后者设置 `TSAN_OPTIONS=halt_on_error=1`。

TSan 使用 `setarch x86_64 -R` 仅启动该次 CTest 子进程；若内核/权限不允许或运行时初始化失败，保留原诊断并报告失败，不修改全局 ASLR、不重试到成功。地址布局与 GNU 包装、全局分配替换测试具有工具相容边界，所以没有把普通完整回归宣称为全项目插桩。现有 Clang 专项每配置有 7 条既有告警：weighted_index.cc 的逐字节 signedness 转换，以及三个 GNU ld 包装测试的 C-linkage 返回 C++ 对象。包装符号用于链接测试，不是对 C 调用者的公共接口；没有新增告警屏蔽，也不声称这些配置无编译告警。

若程序刚启动便报 `FATAL: ThreadSanitizer: unexpected memory mapping`，表示运行时无法建立
所需地址布局，尚不能据此判断项目有数据竞争。先保留直接启动的失败，再验证进程级布局调整：

```sh
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 build/tsan/siftwing-indexer --help
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tsan --output-on-failure --no-tests=error -R '^net[.]production_topology$'
```

这里的 `build/tsan` 须先按上面的独立 TSan 参数构建，并包含所选测试及其 indexer/writer 夹具程序。
将 `setarch` 放在 CTest 外层，使它启动的 Python 和所有受测子进程继承同一设置；仅包装服务程序
不能修复夹具程序的初始化。`Operation not permitted` 说明环境禁止该进程设置，应在允许的环境
执行并如实报告当前未验证。不要通过忽略退出码、禁用插桩、屏蔽报告或随机重试取得通过。

线程资源断言先创建并 join 一个空线程，等待其 `/proc/self/task` 条目消失，再采集基线。
这让 TSan 惰性创建的辅助线程进入基线；仍严格检查生产只增加配置的 worker 数、唯一 I/O
归属及停止后恢复基线，不按线程名称过滤，也不放宽线程数量。

## 格式与静态检查

```sh
python3 -B tools/check_quality.py format
python3 -B tools/check_quality.py tidy --build-dir build/debug
git diff --check
```

需要 clang-format-18 / clang-tidy-18。脚本只读，缺工具、错版本、缺编译数据库或诊断非零即失败。格式基线绑定 `.clang-format` SHA 与 当前仍存在的历史违规文件的精确字节；新增/修改文件须整体符合格式，不能扩充豁免来消除失败。tidy 仅分析 8 个明确生产翻译单元，并检查数据库属于当前源码树。单文件排版可先执行 `clang-format-18 --style=file 文件` 预览，审查后再用 `-i`；不要修改第三方、生成物或原创输入。

## 保存结果与解释失败

在全新结果目录保留：源码提交和必要 diff/文件 SHA、编译器/CMake/系统、完整命令及退出码、配置/构建/CTest stdout/stderr、`compile_commands.json`、`Testing/Temporary/LastTest.log`、失败输入和受测程序 SHA。程序身份须关联真实构建，快照里的 `build_version` 只是一项声明。

Bash 中使用 `tee` 时需开启 `set -o pipefail`，防止日志程序成功掩盖前序失败。保留第一次失败和修复后的结果，不能只记录最后一次成功或将选择列表当作执行结果。GitHub 的 artifact 有保留时限，具体提交是否成功须查对应 run；它不是长期备份，也不意味着仓库设置了分支保护。

如出现协议/业务失败，先区分客户端退出码和远端状态；资源故障应核对 fd/线程及旧快照是否保持。实际服务启动和关闭可按[演示](DEMO.md)复跑；容量、缓存收益和吞吐另按[性能指南](PERFORMANCE.md)测量，不从测试耗时推导生产性能。

需要沿线程/对象定位失败时，按[全项目调试回放](DEBUGGING.md)选择六条核心链路，保存预测、实际观察和故障原件。
