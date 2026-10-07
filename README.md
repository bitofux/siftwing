# Siftwing

Siftwing 是一个 C++17/Linux 文档搜索服务项目，按阶段推进，并以真实工程证据确认进度。

计划先实现确定性的离线索引与查询，再实现 Main-Sub Reactor 网络服务，随后完善可靠性、可观测性、性能，并继续演进分布式能力。每个阶段都须能够构建、测试、调试和解释，再进入下一阶段。

## 项目状态

仓库目前已有最小 CMake/CTest 构建探针：静态库、命令行程序、单元测试与 CLI 集成测试；另有原创、MIT 许可的 TXT/RSS 黄金输入及完整性测试。基础层提供 `Result<T>` / `Result<void>`、拥有诊断文本的 `Error`、受检整数运算，以及文档、词项和快照的强类型身份。文档层提供拥有标题、正文与来源元数据的 `DocumentRecord` 值类型；各层均有运行行为及编译合同测试。读取器、搜索、索引、协议和 Reactor 能力仍属于计划，尚未实现。

## 在 Ubuntu 上构建与测试

需要 CMake 3.20+、C++17 编译器；启用测试时还需要 Python 3.8+。C++ 构建探针与 Python 测试辅助程序仅使用各自标准库，无需下载依赖。

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

探针只统计精确字节，不涉及文本、编码或搜索语义。参数非法时退出 2，输出失败时退出 1。Ubuntu GCC/Clang 配置须实际发现并运行全部十一项测试（两项构建探针、fixtures.integrity、base.result、base.checked、base.id、document.record 及四项编译合同检查），仅编译成功不足以确认验证通过。

基础结果模型位于 [result.h](include/siftwing/base/result.h)，通过 CMake 目标 `siftwing_base` 使用。调用方先检查 `has_value()` 或显式布尔分支，再读取 `value()` / `error()`；访问错分支抛出 `std::bad_variant_access`。结果按载荷类型支持复制或移动构造，不支持赋值；左值访问返回借用引用，右值访问返回拥有值。`[[nodiscard]]` 会诊断直接丢弃结果，显式 `(void)` 仍允许有意忽略。它不自动捕获分配或载荷构造异常。

同一 `siftwing_base` 目标还提供 [checked.h](include/siftwing/base/checked.h) 中的 `checked_add`、`checked_mul` 和 `checked_narrow<To>`。它们先检查整数值域，再执行加法、乘法或转换；不可表示时返回拥有操作上下文的 `Error`。加乘要求同类型整数，支持有符号和无符号；转换覆盖不同宽度和符号，拒绝 `bool`、浮点及枚举。调用前已经发生的隐式截断无法追回；业务容量上限仍由调用者检查。

同一目标的 [id.h](include/siftwing/base/id.h) 提供互不混用的 `DocumentId`、`TermId` 和 `SnapshotVersion`。通过 `Type::from_integer(value, context)` 受检创建 `Result<Type>`，成功后用 `value()` 按值读取底层整数；三者均接受零到 `UINT64_MAX`，无默认身份、整数隐式转换或 ID 算术。文档与词项身份可按数值排序，快照版本仅比较相等，不表示发布时间。身份值不拥有业务对象或视图，也不保证对象存在、编号唯一或快照归属；生成和业务校验由调用方负责。

文档层的 [document_record.h](include/siftwing/document/document_record.h) 通过目标 `siftwing_document` 使用，并传递依赖 `siftwing_base`。`DocumentRecord` 显式接收 `DocumentId`，按值拥有标题、正文和独立的 `DocumentSource`；来源由输入文件相对标识及从 0 开始的文件内逻辑序号定位，另存 TXT/RSS/JSONL 种类和可选 URL、作者、原始发布日期文本。`nullopt` 与已提供的空字符串不同。记录可复制、移动和赋值，不借用解析缓冲区；空文本可表示，生产者负责 UTF-8、来源、容量及可索引性校验。此类型不分配编号、不解析日期、不访问文件或 URL，也不保存哈希、分词或索引结果；创建记录不证明读取成功。

CMake 配置时自动在构建目录生成 `compile_commands.json`；新增源或目标后须重新配置。clangd 等工具可使用所选构建目录的数据库，根目录入口应指向当前配置，生成文件不提交。

[黄金数据说明](tests/fixtures/golden/README.md)记录原创输入、许可证、字节清单和原始 RSS 字段预期。完整性测试核对数据并对损坏副本进行检查，不验证搜索引擎或生产 TXT/RSS 解析器。

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
