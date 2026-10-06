# Siftwing

Siftwing 是一个 C++17/Linux 文档搜索服务项目，按阶段推进，并以真实工程证据确认进度。

计划先实现确定性的离线索引与查询，再实现 Main-Sub Reactor 网络服务，随后完善可靠性、可观测性、性能，并继续演进分布式能力。每个阶段都须能够构建、测试、调试和解释，再进入下一阶段。

## 项目状态

仓库目前已有最小 CMake/CTest 构建探针：静态库、命令行程序、单元测试与 CLI 集成测试。搜索、索引、协议和 Reactor 能力仍属于计划，尚未实现。

## 在 Ubuntu 上构建与测试

需要 CMake 3.20+ 和 C++17 编译器。构建探针只使用 C++ 标准库，无需下载依赖。

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure --no-tests=error

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure --no-tests=error

build/debug/siftwing_build_probe a banana
# 3
```

探针只统计精确字节，不涉及文本、编码或搜索语义。参数非法时退出 2，输出失败时退出 1。两项测试必须被实际发现并运行，仅编译成功不足以确认验证通过。

CMake 配置时自动在构建目录生成 `compile_commands.json`；该生成文件不提交。

使用 GCC/Clang 单独执行 ASan/UBSan 检查：

```sh
cmake -S . -B build/sanitize -DCMAKE_BUILD_TYPE=Debug -DSIFTWING_ENABLE_SANITIZERS=ON
cmake --build build/sanitize --parallel
ctest --test-dir build/sanitize --output-on-failure --no-tests=error
```

## 工程原则

- 优先保证正确性与明确的所有权，避免过早优化。
- 公共合同、失败行为和线程边界须可测试。
- 已验证的工程状态与学习状态分别记录。
- 只发布可复现的构建、测试、调试和性能证据。

## 许可证

Siftwing 使用 MIT 许可证，详见 [LICENSE](LICENSE)。
