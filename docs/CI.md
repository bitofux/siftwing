# Linux自动质量检查

[工作流](../.github/workflows/ci.yml)在`main`推送、PR和手动启动时执行，使用
Ubuntu 24.04的全新runner。工作流只需要公开源码与原创fixture，不读取私有语料、
个人路径或凭据，不启动外部数据库；依赖仍由CMake按[固定归档及哈希](DEPENDENCIES.md)获取。
它是否已在GitHub实际运行、是否成功，须查看具体提交对应的Actions记录；配置文件存在
和本地命令通过都不能代替远端运行证据，也不代表已配置分支保护或必需检查。

## 检查范围

| 独立job | 编译与检查 | 范围 |
|---|---|---|
| GCC Debug / Release | Ninja完整构建、完整CTest | 开启`SIFTWING_BUILD_BENCHMARKS`，包括工具合同测试；不运行性能容量矩阵 |
| Clang 18 ASan/UBSan | 独立Debug、内存/未定义行为检查 | 下列12项真实CTest，启用泄漏检查并在诊断时失败 |
| Clang 18 TSan | 独立Debug、线程检查 | 同12项；仅子进程`setarch x86_64 -R`，不改全局ASLR，不失败重试 |
| LLVM 18静态质量 | 格式门禁、有限clang-tidy | 全部自有C++文件的格式检查，以及8个明确生产翻译单元 |

消毒器专项为：`v3.fault_recovery`、`v3.soak_1`、`v3.soak_2`、`v3.shutdown_races`、
`snapshot.manager`、`snapshot.loader`、`snapshot.publication`、`net.signalfd_online`、
`concurrency.worker_pool_concurrency`、`concurrency.completion_queue_concurrency`、
`cache.versioned_lru_concurrency_test`、`benchmark.loadgen_contract`。
这覆盖快照失败恢复/发布、关闭竞态、线程/队列、LRU及负载端；不把它称作全项目插桩覆盖。
GNU包装和全局分配替换等测试的工具相容性须逐项确认，普通完整回归由GCC两配置承担。
fd压力夹具先在资源充足时取得标准库的预期错误文本，再进入真实EMFILE窗口；这使
消毒器的首次类型检查初始化与业务错误路径分开。它仍检查原Reader错误、旧快照保持及
资源恢复，不证明消毒器自身在首次初始化时已经耗尽fd也能正常工作。
持续运行夹具在两条客户端流都完成首个响应后采集运行基线；资源检查点停止继续发布，
用有限watchdog等待旧请求释放Reader，再要求fd/线程回到精确基线。初始分项观测随测试
JSON保存，不以瞬间数量变化推断泄漏，也不提高资源上限。

编译失败、CTest失败/空列表、工具不可用、静态诊断或门禁脚本错误均使job失败。
工作流显式使用Bash，管道保留`pipefail`行为，不以`tee`成功掩盖前序失败。
checkout和artifact动作固定完整提交SHA，权限仅`contents: read`，checkout不持久化凭据。
同分支新运行可以取消旧运行；构建/test并发有界，job有总超时。

## 本地复跑

使用Ubuntu、CMake、Ninja、GCC、Python 3，以及LLVM 18的Clang/format/tidy。
本机已配置的其他版本不自动安装、替换；GitHub runner在各job显式安装所需系统包。
Debug/Release示例使用不同目录：

```sh
cmake -S . -B build/ci-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSIFTWING_BUILD_BENCHMARKS=ON
cmake --build build/ci-debug --parallel 2
ctest --test-dir build/ci-debug --output-on-failure --no-tests=error --parallel 2

cmake -S . -B build/ci-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DSIFTWING_BUILD_BENCHMARKS=ON
cmake --build build/ci-release --parallel 2
ctest --test-dir build/ci-release --output-on-failure --no-tests=error --parallel 2

python3 -B tools/check_quality.py format
python3 -B tools/check_quality.py tidy --build-dir build/ci-debug
```

消毒器的编译器、CMake参数、精确目标及CTest选择式直接见工作流；ASan/UBSan与TSan
必须使用全新独立目录。TSan的进程地址布局受内核和权限影响；`setarch`或运行时失败应
保留诊断并使检查失败，不能清空日志、关闭工具或重试到成功。上述质量脚本只读，不使用
`clang-format -i`或`clang-tidy --fix`。

## 历史格式债务

现有[clang-format配置](../.clang-format)以LLVM 18为基线。CI接入时300个自有C++文件中，
155个旧文件不符合该配置。本批不对它们做大范围机械重排；
[精确哈希基线](../tools/format_baseline.json)仅允许这些文件在**原字节保持不变**时保留违规。
每次检查仍实际格式化比较全部文件；新增文件或修改后仍不符合格式的文件必须失败。
旧文件一经规范排版即可通过，不要求继续保留旧字节。

基线绑定`.clang-format`的SHA-256和工具主版本；更改配置会失败，需独立审查迁移。
不要把日常“重新生成基线”当作修复。新增/扩大豁免必须给出原因与Review；它不证明全库
已统一排版。减少旧债务可以按模块独立提交，避免和行为变更混在一起。

## 有限静态分析与证据

[clang-tidy配置](../.clang-tidy)启用core/cplusplus分析器及少量明确的bugprone规则，
诊断作为错误；不启用全套风格、命名或现代化建议。
[脚本](../tools/check_quality.py)固定分析fd、信号、移交队列、completion通知、worker、
completion队列、快照manager与loader共8个翻译单元，并要求它们全部出现在当前源码树的
编译数据库中。相关自有头文件参与检查，第三方诊断按SYSTEM头边界处理；其他翻译单元
不在本轮静态分析覆盖范围。扩大规则或范围时应先核实告警和验证成本。

每个job以`always()`上传环境/提交身份、配置/构建/测试或静态检查日志，CTest保留
`Testing/Temporary`，并保存编译数据库，保留7天。上传不替代前序失败状态，网络故障、
取消或runner丢失可能使产物不完整。日志没有无限期保存保证；原始性能样本沿
[性能基准](PERFORMANCE.md)的独立保存流程管理，CI测试不能作为性能结论。
