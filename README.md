# Siftwing

Siftwing 是一个 C++17/Linux 文档搜索服务项目，按阶段推进，并以真实工程证据确认进度。

计划先实现确定性的离线索引与查询，再实现 Main-Sub Reactor 网络服务，随后完善可靠性、可观测性、性能，并继续演进分布式能力。每个阶段都须能够构建、测试、调试和解释，再进入下一阶段。

## 项目状态

仓库目前已有最小 CMake/CTest 构建探针：静态库、命令行程序、单元测试与 CLI 集成测试；另有原创、MIT 许可的 TXT/RSS 黄金输入及完整性测试。基础层提供 `Result<T>` / `Result<void>`、拥有诊断文本的 `Error`、受检整数运算，以及文档、词项和快照的强类型身份。文档层提供拥有标题、正文与来源元数据的 `DocumentRecord`，以及同步 `DocumentReader` 接口、读取报告与有界结果收集；各层均有运行行为及编译合同测试。TXT层提供严格UTF-8校验、一文件一篇、BOM/换行处理与明确失败报告；RSS/JSONL解析、搜索、索引、协议和Reactor能力仍属于计划。

## 在 Ubuntu 上构建与测试

需要 CMake 3.20+、C++17 编译器；启用测试时还需要 Python 3.8+。TXT适配使用固定版本/归档哈希的utfcpp 4.0.6头文件库，首次配置需要网络或已核实的本地依赖源码；组件与BSL-1.0许可说明见[DEPENDENCIES.md](docs/DEPENDENCIES.md)。不安装系统依赖。

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

探针只统计精确字节，不涉及文本、编码或搜索语义。参数非法时退出 2，输出失败时退出 1。Ubuntu GCC/Clang 配置须实际发现并运行全部十六项测试（两项构建探针、fixtures.integrity、六项基础/文档行为测试、TXT文件接入测试及六项编译合同检查），仅编译成功不足以确认验证通过。

基础结果模型位于 [result.h](include/siftwing/base/result.h)，通过 CMake 目标 `siftwing_base` 使用。调用方先检查 `has_value()` 或显式布尔分支，再读取 `value()` / `error()`；访问错分支抛出 `std::bad_variant_access`。结果按载荷类型支持复制或移动构造，不支持赋值；左值访问返回借用引用，右值访问返回拥有值。`[[nodiscard]]` 会诊断直接丢弃结果，显式 `(void)` 仍允许有意忽略。它不自动捕获分配或载荷构造异常。

同一 `siftwing_base` 目标还提供 [checked.h](include/siftwing/base/checked.h) 中的 `checked_add`、`checked_mul` 和 `checked_narrow<To>`。它们先检查整数值域，再执行加法、乘法或转换；不可表示时返回拥有操作上下文的 `Error`。加乘要求同类型整数，支持有符号和无符号；转换覆盖不同宽度和符号，拒绝 `bool`、浮点及枚举。调用前已经发生的隐式截断无法追回；业务容量上限仍由调用者检查。

同一目标的 [id.h](include/siftwing/base/id.h) 提供互不混用的 `DocumentId`、`TermId` 和 `SnapshotVersion`。通过 `Type::from_integer(value, context)` 受检创建 `Result<Type>`，成功后用 `value()` 按值读取底层整数；三者均接受零到 `UINT64_MAX`，无默认身份、整数隐式转换或 ID 算术。文档与词项身份可按数值排序，快照版本仅比较相等，不表示发布时间。身份值不拥有业务对象或视图，也不保证对象存在、编号唯一或快照归属；生成和业务校验由调用方负责。

文档层的 [document_record.h](include/siftwing/document/document_record.h) 通过目标 `siftwing_document` 使用，并传递依赖 `siftwing_base`。`DocumentRecord` 显式接收 `DocumentId`，按值拥有标题、正文和独立的 `DocumentSource`；来源由输入文件相对标识及从 0 开始的文件内逻辑序号定位，另存 TXT/RSS/JSONL 种类和可选 URL、作者、原始发布日期文本。`nullopt` 与已提供的空字符串不同。记录可复制、移动和赋值，不借用解析缓冲区；空文本可表示，生产者负责 UTF-8、来源、容量及可索引性校验。此类型不分配编号、不解析日期、不访问文件或 URL，也不保存哈希、分词或索引结果；创建记录不证明读取成功。

[document_reader.h](include/siftwing/document/document_reader.h) 通过目标 `siftwing_document_reader` 使用。`read_documents` 同步借用调用方已装载的输入字节，按相对输入名字典序调用适配器；`ReadSink::emit` 按条接收拥有型结果，返回 `false` 后适配器须立即停止。报告区分完整、警告、无文字、拒绝和失败；部分提取必须说明未覆盖范围，组装默认策略拒绝，显式允许后仍保留警告。外层 `Result` 成功只表示报告合同成立，调用方还须检查各条状态及 `report.stop`。

调用方显式配置四项正数上限：单输入字节、单篇标题加正文的字节、所有状态结果条数及接纳文本总字节。等于上限合法；单输入/单篇超限拒绝，批量条数/总字节不足则明确停止并保留此前报告，无静默截短。接收端验证来源、严格递增来源序号和已接纳文档 ID 的批量唯一性；不生成编号。接纳限额不等于整个进程内存上限，适配器须在解析/增长前实施自身限制，输入装载成本属于调用方。[原创结果夹具](tests/fixtures/reader_contract/README.md)验证共用交付控制；具体TXT字节适配见下文。尚无RSS/JSONL解析或通用文件装载入口。

[txt_reader.h](include/siftwing/document/txt_reader.h)通过目标`siftwing_txt_reader`使用。`TxtDocumentReader`按值拥有输入名到`TxtDocumentMetadata`的映射：调用方显式提供文档ID和可选标题，不自动编号；相同输入与配置可确定地复读。请求名未配置会抛出`std::out_of_range`，属于调用方配置违约，不伪装成文件失败。它沿用`read_documents`的已装载字节接口，不遍历目录或负责文件装载。

TXT默认一文件一篇，作者、出处、章节和标记作为正文保留。严格校验UTF-8、拒绝NUL；仅去除开头UTF-8 BOM，将CRLF/CR变为LF，不裁剪正常正文。空或全为Unicode15.1 White_Space的正文报告无文字。非空白显式标题原值优先，否则用文件名去最后扩展名并报告回退警告。最终标题加正文的字节预算在正文分配前检查，超限拒绝整篇，不截断；原始输入预算仍包含BOM和原换行。非法编码、标题和大小问题有定位或原因，返回报告拥有文本，不依赖输入或适配器存活。这里的读取是字节适配，文件装载前容量和文件系统路径安全仍由调用方负责。

CMake 配置时自动在构建目录生成 `compile_commands.json`；新增源或目标后须重新配置。clangd 等工具可使用所选构建目录的数据库，根目录入口应指向当前配置，生成文件不提交。

启用测试时还生成 `siftwing_txt_corpus_probe` 专用文件接入验收探针：

```sh
build/debug/siftwing_txt_corpus_probe /ABS/INPUT_ROOT 67108864 16777216 100 33554432 a.txt notes/b.txt
```

参数依次为绝对输入根、单输入字节、单篇文本字节、结果条数、累计文本字节及显式相对输入名。
探针按输入名排序，由这个调用方从0分配本批局部ID；一文件一篇，使用文件名回退标题。
逐组件打开时拒绝符号链接，只接纳普通文件，分配前检查输入大小；一次只装载当前文件，
读取后核对大小和变动时点。缺文件、链接、非常规文件或IO错误有失败报告，普通失败继续，
批量容量不足仍明确停止。调用期间输入须保持不变；这些检查不提供原子快照或恶意并发修改保证。
stdout输出验收用schema1 JSON，字符串使用hex表示字节。退出0仅表示报告可信，还须检查
`counts`、逐条状态和`stop`；请求/根配置错误退出2，未知异常或输出失败退出1。
该探针仅用于验收，不是通用生产加载API或正式建库入口，其JSON与枚举数值不是生产持久化格式。
`document.txt_file_ingestion`用临时原创文件验证整篇/长文、容量、拥有性、确定性及路径/IO失败；
私有数据不进入公开测试，报告与抽取产物应保存到输入根之外。

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
