# Siftwing

Siftwing 是一个 C++17/Linux 文档搜索服务项目，按阶段推进，并以真实工程证据确认进度。

[V2工程基线](docs/V2.md)汇总公开输入的四命令运行、协议与验收矩阵、小型loopback基准和交接限制。

已实现单机离线索引与本地查询，当前能力、验收入口及限制见 [V1基线](docs/V1.md)，两条调用链的调试、故障和修改练习见 [V1调试学习包](docs/V1_DEBUGGING.md)。Main-Sub Reactor 已接通正式网络搜索服务，当前采用[单连接一个在途请求](docs/SINGLE_INFLIGHT.md)，已有[输出高低水位与背压](docs/OUTPUT_BACKPRESSURE.md)和[worker队列饱和拒绝](docs/WORKER_SATURATION.md)；已接入[可选空闲超时与请求deadline](docs/DEADLINES.md)，默认关闭、到期局部关闭连接；业务层支持[可选版本感知LRU](docs/VERSIONED_LRU.md)，默认关闭，成功Body有界缓存并隔离全部结果参数。已有[网络客户端](docs/NETWORK_CLIENT.md)支持PING、SERVER_INFO、SEARCH、SUGGEST，连接/发送/接收共用固定总期限。现有[严格JSON启动配置](docs/SERVER_CONFIG.md)可配置监听地址、线程/队列、路径、缓存、期限与关闭预算，按默认值→文件→CLI合并。[结构化日志](docs/LOGGING.md)关联线程、连接代次、请求与查询阶段，默认以`info`写入stderr，可显式关闭；指定日志文件后自动按大小轮转，默认10MiB、3份归档加当前文件。已有[最小指标](docs/METRICS.md)，持续计量连接、队列、缓存、请求、错误、查询延迟和索引，并按日志级别导出周期及关闭样本。后续完善资源控制、可靠性和性能；各阶段按独立门禁推进。

V2 的 [帧头与字节序合同](docs/PROTOCOL.md)定义 24 字节布局、消息编号、长度和错误边界，
并提供黄金字节例子；已有[有界连续 Buffer](docs/BUFFER.md)和[增量帧解码](docs/FRAME_DECODER.md)，
另有[帧编码、Body准入与统一响应](docs/FRAME_CODEC.md)及[codec黄金/损坏输入验证](docs/CODEC_TESTING.md)、
[fd RAII与socket基线](docs/SOCKET_BASELINE.md)、[epoll创建与LT注册](docs/EPOLL_REGISTRATION.md)及
[Listener与有界accept4接入](docs/LISTENER.md)、[Connection有界读取与请求准入](docs/CONNECTION_READ.md)
及[有界响应输出、部分写与写事件意向](docs/CONNECTION_WRITE.md)。
已有[单循环协议探针](docs/PROTOCOL_LOOP.md)接通 PING/PONG 与错误响应；生产业务搜索网络服务见[在线搜索闭环](docs/ONLINE_SEARCH.md)，Main-Sub协议组合见后文。正式服务支持[优雅退出](docs/GRACEFUL_SHUTDOWN.md)：停止接入/任务准入，限时排空已在途响应，再完成worker与通知fd屏障；截止不取消任务或保证线程join时长。故障、并发与工具证据边界见[V2 测试矩阵](docs/V2_TESTING.md)。
Main/Sub协作遵循[线程拓扑与所有权合同](docs/THREAD_TOPOLOGY.md)，明确 Main/Sub 职责、fd 转交和停止清理边界。
已有独立[Sub Reactor生命周期基线](docs/SUB_REACTOR.md)：独立线程、线程内epoll资源、就绪报告、有限等待停止和外部join；该类型保持空生命周期，连接、队列和eventfd由[ProtocolSubReactor](docs/MULTI_REACTOR.md)组合。
已有独立[fd有界移交队列](docs/FD_HANDOFF_QUEUE.md)：预分配FIFO、成功消费源、拒绝保持源、同步关闭入口与关闭后drain；专用Sub接入见多Sub协议集成。
已有独立[eventfd新连接唤醒](docs/EVENTFD_WAKEUP.md)：入队后通知、合并计数与真实队列分离、有限drain；通知失败不撤销已提交fd。
已有独立[Main Reactor轮询分配](docs/MAIN_REACTOR.md)：有限接入、固定目标移交、满队列不改投、跨线程停止通知与有限等待后备。
已有[多Sub协议集成](docs/MULTI_REACTOR.md)：ProtocolSubReactor与Main组合，真实TCP验证固定Connection归属、协议响应、发布者静止屏障和失败回滚；worker与生产搜索服务命令已通过[在线闭环](docs/ONLINE_SEARCH.md)接入。

已有独立[有界TaskQueue](docs/TASK_QUEUE.md)：独占任务提交、满/关闭时保留源、消费者谓词等待与关闭排空；已被WorkerPool与[真实查询任务](docs/ONLINE_SEARCH.md)复用。

固定线程WorkerPool的就绪、任务异常隔离与关闭排空合同见[组件说明](docs/WORKER_POOL.md)。独立[SearchTask/Completion载荷](docs/SEARCH_TASK.md)已定义拥有请求与响应，独立[ConnectionId/generation与路由包装](docs/CONNECTION_IDENTITY.md)已实现，并迁移Sub注册元数据；独立[CompletionQueue](docs/COMPLETION_QUEUE.md)已有有界结果归属/发布/关闭排空合同；[eventfd结果回投](docs/COMPLETION_WAKEUP.md)已接入ProtocolSubReactor的可选请求提交器，worker不访问连接；真实SEARCH/SUGGEST与生产服务命令已通过[在线搜索闭环](docs/ONLINE_SEARCH.md)接入。

## 项目状态

查询层已有[Query模型](docs/QUERY.md)：复用建库文本管线，拥有过滤后词项/重复词频，
校验AND/OR和top_k，并区分空查询错误与未知词。[候选集合](docs/CANDIDATES.md)已按AND/OR
合并倒排，返回拥有的递增唯一文档身份。[余弦排序](docs/RANKING.md)组合同索引候选与
单位TF-IDF权重，返回完整分数降序、同分DocId升序结果；[稳定top-k](docs/TOP_K.md)复用完整排序返回前k条，
保留完整准入与精确分数。[动态摘要](docs/SUMMARY.md)围绕原文首个查询词截取UTF-8标量窗口，
保留原文字节与偏移，缺失正文返回空摘要。[关键词高亮](docs/HIGHLIGHT.md)合并原文重叠及相邻命中，
完整转义正文后生成固定mark标签，并返回原字节区间。[候选词推荐](docs/RECOMMENDATION.md)使用
独立字符索引召回、Unicode编辑距离和推荐词频稳定取前k；正式离线建库由`siftwing-indexer`提供，
本地SEARCH/SUGGEST由[siftwing-cli](docs/CLI.md)从完整快照独立执行。

仓库提供完整离线建库与查询应用、组件行为/编译合同测试和端到端黄金集成测试；另保留最小 CMake/CTest 构建探针及原创、MIT 许可的 TXT/RSS 黄金输入。基础层提供 `Result<T>` / `Result<void>`、拥有诊断文本的 `Error`、受检整数运算，以及文档、词项和快照的强类型身份。文档层提供拥有标题、正文与来源元数据的 `DocumentRecord`，以及同步 `DocumentReader` 接口、读取报告与有界结果收集；各层均有运行行为及编译合同测试。TXT层提供严格UTF-8校验、一文件一篇、BOM/换行处理与明确失败报告；RSS层已提供RSS2字节适配、字段选择、HTML正文抽取与拥有型报告；文本层提供严格UTF-8的共用规范化helper，以及共享Tokenizer接口、有界ASCII英文/数字分词、cppjieba中文/混合分词、停用词过滤/词频统计和统一文本管线、基于TF的64位SimHash指纹及独立批量去重决策；Query模型、候选集合、余弦排序、稳定top-k、动态摘要、关键词高亮与候选词推荐已提供，本地完整检索执行由正式CLI提供，增量拆帧、帧编码、JSON对象准入和统一响应已提供，Main-Sub协议连接接入已有独立组合验证，业务字段验证和搜索网络服务已由[在线搜索闭环](docs/ONLINE_SEARCH.md)提供，线格式合同见[协议规范](docs/PROTOCOL.md)。当前输入计划聚焦TXT与RSS XML；JSONL解析/导入已取消，独立HTML、PDF、DOCX、Excel和Markdown暂留后续扩展。

## C++ 代码格式化

根目录的 [.clang-format](.clang-format) 使用 LLVM 基础风格、C++17、四空格缩进和 100 列，
展开短控制语句及非空函数，保留中文结构化合同注释、include/using 顺序和限定符。
格式工具以 Ubuntu `clang-format 18` 验证；clang-format 的主版本独立于项目的 C++17 标准。
项目自有实现使用 `.cc`，头文件使用 `.hpp`；核心代码已格式化，测试保留原排版。

从仓库根目录操作单个自有文件：

```sh
clang-format --version
clang-format --style=file --fallback-style=none src/query/top_k.cc
clang-format --style=file --fallback-style=none -i src/query/top_k.cc
clang-format --style=file --fallback-style=none --dry-run --Werror src/query/top_k.cc
git diff --check
git diff -- src/query/top_k.cc
```

第二条命令只向终端输出预览，`-i` 才会改写文件；`--dry-run --Werror` 检查排版是否一致，
现有未格式化文件可能返回非零。先检查 diff，再按变更验证，不用排版检查替代业务测试。
只对 `include/`、`src/`、`apps/` 及自有测试代码按文件使用格式化器；生成物、第三方、
私有资料和黄金输入保持原样。
clangd 18 支持读取同一配置，编辑器可通过其格式化功能应用规则；本配置不设置自动保存行为。

## 在 Ubuntu 上构建与测试

需要 CMake 3.20+、C++17 编译器；启用测试时还需要 Python 3.8+。文本处理使用utfcpp 4.0.6，HTML片段使用Lexbor 3.0.0，RSS XML使用tinyxml2 11.0.0，中文分词使用cppjieba 5.6.7及固定limonp子模块，结构化日志使用spdlog 1.15.3及bundled fmt 11.2.0；依赖均固定版本及归档哈希。首次配置需要网络或已核实的本地依赖源码；来源、许可与离线配置见[DEPENDENCIES.md](docs/DEPENDENCIES.md)。不安装系统依赖。

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

探针只统计精确字节，不涉及文本、编码或搜索语义。参数非法时退出 2，输出失败时退出 1。Ubuntu GCC/Clang 配置须实际发现并运行全部已注册测试（基础/文档/文本/索引行为、接入/独立参考及公共头编译合同检查），仅编译成功不足以确认验证通过。

基础结果模型位于 [result.hpp](include/siftwing/base/result.hpp)，通过 CMake 目标 `siftwing_base` 使用。调用方先检查 `has_value()` 或显式布尔分支，再读取 `value()` / `error()`；访问错分支抛出 `std::bad_variant_access`。结果按载荷类型支持复制或移动构造，不支持赋值；左值访问返回借用引用，右值访问返回拥有值。`[[nodiscard]]` 会诊断直接丢弃结果，显式 `(void)` 仍允许有意忽略。它不自动捕获分配或载荷构造异常。

同一 `siftwing_base` 目标还提供 [checked.hpp](include/siftwing/base/checked.hpp) 中的 `checked_add`、`checked_mul` 和 `checked_narrow<To>`。它们先检查整数值域，再执行加法、乘法或转换；不可表示时返回拥有操作上下文的 `Error`。加乘要求同类型整数，支持有符号和无符号；转换覆盖不同宽度和符号，拒绝 `bool`、浮点及枚举。调用前已经发生的隐式截断无法追回；业务容量上限仍由调用者检查。

同一目标的 [id.hpp](include/siftwing/base/id.hpp) 提供互不混用的 `DocumentId`、`TermId` 和 `SnapshotVersion`。通过 `Type::from_integer(value, context)` 受检创建 `Result<Type>`，成功后用 `value()` 按值读取底层整数；三者均接受零到 `UINT64_MAX`，无默认身份、整数隐式转换或 ID 算术。文档与词项身份可按数值排序，快照版本仅比较相等，不表示发布时间。身份值不拥有业务对象或视图，也不保证对象存在、编号唯一或快照归属；生成和业务校验由调用方负责。

文档层的 [document_record.hpp](include/siftwing/document/document_record.hpp) 通过目标 `siftwing_document` 使用，并传递依赖 `siftwing_base`。`DocumentRecord` 显式接收 `DocumentId`，按值拥有标题、正文和独立的 `DocumentSource`；来源由输入文件相对标识及从 0 开始的文件内逻辑序号定位，另存 TXT/RSS/JSONL 种类和可选 URL、作者、原始发布日期文本。JSONL在这里仅为既有来源标识，不表示已实现解析，也不列入当前导入计划。`nullopt` 与已提供的空字符串不同。记录可复制、移动和赋值，不借用解析缓冲区；空文本可表示，生产者负责 UTF-8、来源、容量及可索引性校验。此类型不分配编号、不解析日期、不访问文件或 URL，也不保存哈希、分词或索引结果；创建记录不证明读取成功。

[document_reader.hpp](include/siftwing/document/document_reader.hpp) 通过目标 `siftwing_document_reader` 使用。`read_documents` 同步借用调用方已装载的输入字节，按相对输入名字典序调用适配器；`ReadSink::emit` 按条接收拥有型结果，返回 `false` 后适配器须立即停止。报告区分完整、警告、无文字、拒绝和失败；部分提取必须说明未覆盖范围，组装默认策略拒绝，显式允许后仍保留警告。外层 `Result` 成功只表示报告合同成立，调用方还须检查各条状态及 `report.stop`。

调用方显式配置四项正数上限：单输入字节、单篇标题加正文的字节、所有状态结果条数及接纳文本总字节。等于上限合法；单输入/单篇超限拒绝，批量条数/总字节不足则明确停止并保留此前报告，无静默截短。接收端验证来源、严格递增来源序号和已接纳文档 ID 的批量唯一性；不生成编号。接纳限额不等于整个进程内存上限，适配器须在解析/增长前实施自身限制，输入装载成本属于调用方。[原创结果夹具](tests/fixtures/reader_contract/README.md)验证共用交付控制；具体TXT字节适配见下文。RSS字节适配见下文。正式[indexer](docs/INDEXER.md)负责受检文件装载并组装该字节接口；Reader组件本身不负责文件遍历或装载。当前不实施JSONL解析/导入。

[txt_reader.hpp](include/siftwing/document/txt_reader.hpp)通过目标`siftwing_txt_reader`使用。`TxtDocumentReader`按值拥有输入名到`TxtDocumentMetadata`的映射：调用方显式提供文档ID和可选标题，不自动编号；相同输入与配置可确定地复读。请求名未配置会抛出`std::out_of_range`，属于调用方配置违约，不伪装成文件失败。它沿用`read_documents`的已装载字节接口，不遍历目录或负责文件装载。

TXT默认一文件一篇，作者、出处、章节和标记作为正文保留。严格校验UTF-8、拒绝NUL；仅去除开头UTF-8 BOM，将CRLF/CR变为LF，不裁剪正常正文。空或全为Unicode15.1 White_Space的正文报告无文字。非空白显式标题原值优先，否则用文件名去最后扩展名并报告回退警告。最终标题加正文的字节预算在正文分配前检查，超限拒绝整篇，不截断；原始输入预算仍包含BOM和原换行。非法编码、标题和大小问题有定位或原因，返回报告拥有文本，不依赖输入或适配器存活。这里的读取是字节适配，文件装载前容量和文件系统路径安全仍由调用方负责。

文档库的单记录编码、结构/CRC 校验和借用输出流写入由 `siftwing_record_format` 提供，
详见[记录格式 v1](docs/RECORD_FORMAT.md)。长度预算包含框架开销，缺失与已提供空元数据保持区别。
写入失败可能留下前缀，调用方必须废弃临时输出并检查最终 flush/close；该记录组件不承担
偏移表、随机读取或快照发布，记录写入成功不代表搜索索引已完成。

`siftwing_document_store` 提供[偏移表与随机读取](docs/DOCUMENT_STORE.md)：只读扫描生成
按 DocId 排序的 SWDI v1 表，检查整库覆盖，再用拥有只读 fd 的读者校验全部记录对应关系。
随机读取使用 pread，返回独立拥有的文档；拒绝越界、截断、损坏和错位身份。文件须由调用方
保持稳定；完整快照的清单、发布与加载由下文的snapshot目标组合提供。

`siftwing_lexicon` 提供[固定词表与推荐字符索引](docs/LEXICON.md)。固定唯一词表按 UTF-8
字节序从 0 编号，同词集合的顺序/重复不影响 TermId；词表变化可以改编号，ID 属于对应
词典/快照。搜索 Lexicon 与推荐词典独立，后者保存显式选择语料的累计词频和 Unicode
标量→唯一词项集合，不默认搜索去重，不计算 DF 或文档倒排。输入词键须由上游产生，
本组件严格校验但不隐式规范化/过滤；提供确定性内存枚举，快照持久化由Writer/Reader提供，
候选查询由[推荐模块](docs/RECOMMENDATION.md)消费，推荐结果不依赖搜索词典。

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

[html_text.hpp](include/siftwing/document/html_text.hpp)通过目标`siftwing_html_text`提供
`extract_html_text`，同步借用UTF-8 HTML片段，返回拥有正文和解析恢复标记的`Result<HtmlText>`。
它供下述RSS适配器处理正文字段，尚无独立HTML文件入口。
实体由Lexbor按HTML规则还原一次；普通空白折叠、块元素和`br`保留段落边界，`pre`保留
解析后的空白。`script/style/template/noscript`、嵌入资源及`hidden`子树过滤，链接只取文字；
不执行JS、解释CSS或加载URL/图片。空正文明确为空字符串，HTML语法恢复由`recovered`表示。

调用方显式配置输入字节、输出字节、token、DOM节点和深度五项正预算；等于上限合法，
超限返回有上下文的失败，不交付截短正文。输入/解析/结构/输出分别检查，节点预算在解析后
检查，这些限额不等于进程内存或CPU硬上限。公共行为、编译合同和原创黄金RSS字段集成
通过真实CTest注册；字段优先及回退由下述RSS适配器实施。使用的utfcpp、Lexbor和tinyxml2版本、来源、
哈希和许可证集中记录在[DEPENDENCIES.md](docs/DEPENDENCIES.md)。

[rss_reader.hpp](include/siftwing/document/rss_reader.hpp)通过目标`siftwing_rss_reader`提供
`RssDocumentReader`和`RssParseLimits`。调用方按输入名显式提供每个原item位置的ID向量，
包括无正文和拒绝位置；缺配置抛`std::out_of_range`，成功解析后的item数不匹配抛
`std::invalid_argument`，数量检查在发布任何item前完成。不隐式生成生产身份。

适配器接纳无命名空间RSS2的`rss version="2.0"/channel/item`，只解析已装载字节。
正文依次选择content命名空间URI的`encoded`、无命名空间本地`content`、`description`；
前缀可改名，按URI解析最近祖先声明。字段连接全部直接Text/CDATA；缺失或抽取空才回退，
选用非首选字段有警告。首选失败或超限拒绝该item，不用后续摘要掩盖；消费字段含XML元素
孩子或已识别字段重复也明确拒绝。未知扩展忽略。title只作XML解码纯文本并裁剪外空白，
空标题用文件stem加`#`和原0基ordinal；link、优先dc:creator否则author及pubDate单独拥有，
日期原文不解析。HTML恢复保留警告；正文空为no_text，不交付部分正文。

XML使用tinyxml2建树，项目在其前后检查严格UTF-8、XML1.0字符/实体/声明和命名空间；
拒绝DTD，不访问外部实体、文件或URL。坏XML整文件失败，不发布前面item；不支持Atom/RDF
或任意转换XML。调用方提供XML节点/深度及HTML五项正预算，另沿用共用读取四预算。
XML节点/深度在DOM建立后、逐item前检查，tinyxml2另有500层内部限制；这些预算不提供
进程内存或CPU硬上限。接收端false后立即停止下一item，最终正文/元数据不借用DOM或输入。

启用测试时的`siftwing_rss_corpus_probe`从stdin受限装载单份XML，以参数显式给定原item数：

```sh
build/debug/siftwing_rss_corpus_probe 3 1048576 1048576 1000 8388608 100000 128 1048576 1048576 100000 100000 128 < tests/fixtures/golden/rss/original.xml
```

参数为item数、共用四预算、XML节点/深度、HTML输入/输出字节/token/节点/深度。
仅该验收调用方按位置配置0起ID；stdout为schema1 hex字符串JSON，退出0仍须查看各状态和
stop，输入装载或报告合同失败退出1，参数/身份数量违约退出2。它不提供生产文件加载API，
输出不是持久化格式。`document.rss_ingestion`使用原创黄金输入及独立手写文字预期，
不依赖私有语料；私有报告须保存到输入根之外。

[normalize.hpp](include/siftwing/text/normalize.hpp)通过目标`siftwing_text_normalization`提供
`normalize_utf8(input, limits)`，返回拥有型`Result<std::string>`。规则版本
`normalization_policy_version=1`：ASCII A—Z转小写，Unicode15.1 White_Space折成单个ASCII
空格并去首尾空白，CR/LF也折空格；数字、标点、中文、非ASCII大小写和其他合法码点原样保留。
它不做完整Unicode case folding或NFC/NFKC；U+FEFF不是该空白集合，在所有位置保留，文件
BOM处理属于适配器。该规则与locale无关，足够预算下重复规范化不变，不删除标点拼接词语。

调用方提供输入和最终输出两项正字节预算，等于上限合法。配置/输入大小先检查，再完整
UTF-8/NUL校验，最后预计算最终输出大小并分配；失败不含部分字符串，Error.context定位
`normalize.limits/input_bytes/utf8/nul/output_bytes`，非法序列/NUL诊断含0基字节位置。
空/全空白输入成功为空串，分配/未知异常上传。输入同步借用，输出拥有且原DocumentRecord
保持原值；独立调用无共享可变状态，无文件或网络访问，预算不是全进程资源硬限。

公开单元验证规则/临界/拥有性，Python独立参考覆盖除NUL外全部合法Unicode标量及原创混合
文本；接入测试用实际TXT/RSS适配器生成记录后显式调用同一helper。该接线测试只验证规范化，
英文分词与统一文本管线见下文；正式离线建库见[INDEXER.md](docs/INDEXER.md)，已实现的本地查询见[CLI.md](docs/CLI.md)。启用测试时的`siftwing_normalize_probe`支持
`INPUT_BYTES OUTPUT_BYTES < text`，stdout为版本加hex字符串JSON；退出0表示成功，失败或
装载超限1、配置错误2，仅为验收工具，不是生产装载/持久化接口。utfcpp来源/版本/许可仍见
[DEPENDENCIES.md](docs/DEPENDENCIES.md)，本模块没有新增第三方库。

[tokenizer.hpp](include/siftwing/text/tokenizer.hpp)提供同步`Tokenizer`抽象与拥有型
`TokenSequence`（`vector<string>`）；[english_tokenizer.hpp](include/siftwing/text/english_tokenizer.hpp)
中的`EnglishTokenizer`通过目标`siftwing_tokenizer`实现同一接口，由正式建库和查询通过共享文本管线复用。
英文规则版本`english_tokenization_policy_version=1`：最大连续`[A-Za-z0-9]+`为token，
保留顺序、重复和大小写；标点、空白及全部非ASCII字符作为分隔，不产生空token。
例如`don't`→`don/t`、`a1`保持一个词、`3.14`→`3/14`、`foo中文bar`→`foo/bar`、
`café`→`caf`。它不音译、不做Unicode词边界或中文分词；大小写由调用方先显式规范化。

调用方提供四项正预算：输入UTF-8字节、token条数、单token字节、所有token字节之和；
重复词按条计数，分隔符不计输出，等于上限合法。配置/输入字节→完整UTF-8/NUL→
按源token顺序检查单token/条数/累计字节→分配/复制；非法编码不会被输出超限掩盖。
空/仅分隔符/纯非ASCII输入成功为空序列，failure不交付部分词。Error.context为
`tokenizer.limits/input_bytes/utf8/nul/token_bytes/tokens/output_bytes`，编码/NUL含0基
字节位置。输入同步借用且不能被并发写入；输出独立拥有，未知分配/标准库异常上传。
英文实现无共享可变状态，抽象接口的const方法不自动保证未来派生实现线程安全。
限额不含容器/分配器开销，不是进程内存或CPU硬限。

原创测试验证多态、词边界、预算/失败优先级与拥有性，Python独立ASCII正则覆盖全部合法
非NUL Unicode标量及混合序列；TXT/RSS抽取与查询形状文字在测试中显式规范化后调用同一
Tokenizer；统一管线、停用词及词频见后文，内存索引由倒排与权重组件提供；正式本地检索由[siftwing-cli](docs/CLI.md)提供，网络服务由[在线搜索闭环](docs/ONLINE_SEARCH.md)提供。
验收工具`siftwing_english_tokenizer_probe INPUT_BYTES TOKENS TOKEN_BYTES OUTPUT_BYTES < text`
仅在启用测试时构建，stdout为规则版本及`tokens_hex`数组JSON；成功0、输入/内容失败1、
参数错误2。它不隐式规范化，也不提供生产装载/持久化合同。本模块无新第三方库，
继续复用[utfcpp记录](docs/DEPENDENCIES.md)。

[cppjieba_tokenizer.hpp](include/siftwing/text/cppjieba_tokenizer.hpp)新增`CppJiebaTokenizer`，
目标`siftwing_cppjieba_tokenizer`实现同一`Tokenizer`。工厂`create(CppJiebaConfig)`接收
主词典/HMM/可选单一用户词典的绝对普通文件路径及正`max_file_bytes`，返回
`Result<unique_ptr<CppJiebaTokenizer>>`；默认启用HMM。路径各组件禁止符号链接，
资源完整UTF-8/NUL/格式/数值验证后从内存初始化，失败返回`cppjieba.*`诊断。
已建对象拥有资源，之后文件变化不影响它；新对象读取本次文件，不按路径缓存。

最大连续汉字段交cppjieba，ASCII仍为`[A-Za-z0-9]+`，其他字符分隔；汉字范围为
U+3007和头文件明示的固定CJK块，非完整Unicode Han属性。保留大小写/顺序/重复，
调用方先显式规范化。四项token预算与失败无前缀合同沿用；另有`INT_MAX/4`实际输入
字节限制以保护库内部索引。库临时分词内存不受输出预算硬限制。
例如原创词典中的“中文A1中国”产生“中文/A1/中国”；实际中文边界随资源及HMM变化。

测试覆盖原创词典/HMM/user、初始化坏资源、预算、拥有性、同路径更新、独立参考和
TXT/RSS显式接线。验收探针参数为
`siftwing_cppjieba_tokenizer_probe INPUT_BYTES TOKENS TOKEN_BYTES OUTPUT_BYTES DICT HMM USER_OR_DASH FILE_BYTES HMM_ON < text`，
`USER_OR_DASH`为`-`则无用户词典，`HMM_ON`为`0`或`1`；输出/退出约定沿英文探针。
使用的cppjieba5.6.7、limonp固定提交、许可证、内存补丁、词典来源及SHA详见
[第三方依赖记录](docs/DEPENDENCIES.md)。共用管线与停用词见后文，索引由[倒排组件](docs/INVERTED_INDEX.md)和[权重组件](docs/WEIGHTS.md)提供。

[term_frequency.hpp](include/siftwing/text/term_frequency.hpp)与目标`siftwing_term_frequency`
提供内存停用词配置、过滤及统计。`StopWords::parse`合并显式UTF-8字节源：每源首BOM
剥离，LF/CRLF分行，复用ASCII小写/Unicode White_Space折叠及trim，空白行忽略，重复
折叠；内部空白拒绝。`#`和标点为字面词条，不作注释，不把条目再次分词。调用者承担文件
加载及配置身份，空源允许禁用停用词；正配置预算约束原输入/唯一条数/词条和词表字节。

`analyze_tokens`接收已经显式规范化/分词的拥有序列和只读StopWords；按完整token字节
精确匹配，保留顺序、大小写和重复，返回拥有型`TokenAnalysis`（过滤序列+单文档TF）。
例如停用词`the/的`、输入`the/中国/中国/的/a1`得到`中国/中国/a1`，TF中国2、a1 1。
过滤不隐式lower，输入`THE`仍保留；所有输入token包括将被滤掉的词都检查UTF-8/NUL、
非空/无Unicode White_Space及单词预算。七正预算分别约束输入条数/字节、单token字节、
输出条数/字节和TF唯一词数/键字节；完整编码检查先于输出超限，失败无部分序列或统计。

`accumulate_recommendation`将一个文档TF合入`RecommendationFrequencies`并返回新表，
输入不变；所有次数为受检uint64，外部表也验证正频、合法词键和total_tokens总和一致。
`accumulate_recommendation_batch`从空统计累计整批TF，在局部词表中合并并一次返回；
不逐篇校验或复制已有累计表，失败仍无部分结果，输入保持不变。
推荐累计按调用者显式选择的文档交付次数统计，空文档也计1，重复交付重复累计，不隐式
SimHash去重。例如再交付`中国`得到推荐中国3、a1 1；搜索DF中国2属于后续去重文档集合
的统计，本模块不计算DF/N/TF-IDF或词项ID。五正累计预算覆盖文档次数/唯一词数/单词字节/
词表字节/总出现次数，溢出拒绝且不改变已有表。map/set按UTF-8字节字典序稳定遍历。

本模块无文件/网络IO、生产文本管线或新增第三方。复用已有normalize与utfcpp，来源/许可
见[依赖记录](docs/DEPENDENCIES.md)。分配/未知异常上传，预算不含容器开销，不是进程硬
资源限；累计复制已有词表，当前未声称性能优化。原创手写黄金、Python独立Counter、
公共头合同及TXT/RSS测试显式接线验证行为。测试探针`siftwing_term_frequency_probe EN_OR_DASH CN_OR_DASH`
仅作验收：stdin每文档以`@`行开始、其后每行一个UTF-8 token的hex，stdout为停用词集合、
过滤序列、各文档TF与推荐累计JSON；输入/资源/内容失败1、参数/hex错误2、成功0。
它有独立装载预算，不提供生产配置或持久化合同。

使用 GCC/Clang 单独执行 ASan/UBSan 检查：

## 共用文本管线

[text_pipeline.hpp](include/siftwing/text/text_pipeline.hpp)通过目标`siftwing_text_pipeline`使用。
`TextPipeline::create(unique_ptr<Tokenizer>, StopWords)`接管已初始化分词器并保存拥有型停用集合，
成功返回非空`unique_ptr<TextPipeline>`，空分词器失败为`pipeline.tokenizer`；对象不复制/移动，
以指针转移拥有权。工厂不读取分词资源或停用词文件，资源装载沿各组件原合同由调用者完成。

建库正文和查询文字都调用`analyze(string_view, const TextPipelineLimits&)`：完整正配置预检后，
依次执行`normalize_utf8`→`Tokenizer::tokenize`→`analyze_tokens`，返回拥有的有序保留词序列和
单文档TF。三组2/4/7正预算分别计原文/规范化文字、未过滤token和保留序列/唯一词键；
建库与查询可设不同上限，预算充分时使用同一规则、分词资源和停用集合得到相同词项。
零预算返回`pipeline.limits`；其余阶段失败保留原诊断且停止下游，不发布部分结果，分配/未知异常上传。

空文字、无词或全过滤在文本层成功返回空分析；查询合法性由上述Query层判断。输入正文/标题和
来源元数据不修改，返回结果不借用输入或管线；不隐式拼接标题、累计推荐词频、生成DF/身份或写索引。
线程安全依赖所选Tokenizer的实际合同，`const`本身不提供保证；预算不是整个进程硬资源限。
管线不保存快照配置摘要；该身份由[indexer](docs/INDEXER.md)组装并写入manifest。管线不提供运行时替换配置。同一入口集成测试覆盖原创TXT/RSS/查询，
独立参考校验英文Unicode边界及手算中文黄金；离线建库见[INDEXER.md](docs/INDEXER.md)，本地查询见[CLI.md](docs/CLI.md)。

[simhash.hpp](include/siftwing/document/simhash.hpp)通过目标`siftwing_simhash`提供
`make_simhash(DocumentTermFrequencies, SimHashLimits)`和`hamming_distance`。
前者消费调用方明确选择的、过滤后完整单文档TF，不重新规范化/分词/过滤，不使用推荐累计或搜索DF。
规则版本`simhash_policy_version=1`：每个唯一词按UTF-8字节作FNV-1a64后接fmix64，原始TF为整数权重，
每位正票严格多数置1、平票0。固定常量及顺序在公共头中定义，不依赖`std::hash`、locale或浮点。

输入须为严格UTF-8、无NUL/空词/Unicode15.1 White_Space的正词频表，次数之和等于`total_tokens`。
四项正预算分别限制唯一词数、单词字节、唯一词键总字节与总TF；等于上限合法，支持总TF至`UINT64_MAX`。
配置、唯一词数、词键字节预算、完整编码、词键/正频/总和、总次数预算依次检查。
成功返回拥有型`optional<SimHash64>`；空表且总数0返回`nullopt`，非空指纹数值0仍合法。
普通错误返回`simhash.*`诊断且无部分指纹，诊断分配/未知异常上传；输入同步借用，结果不借用输入。

计算使用固定64项栈票数，工作量为词键字节扫描加64×唯一词数，不按重复次数展开；
预算不限制调用方已装载TF/管线临时对象或进程总资源。函数无IO/共享可变状态，不修改`DocumentRecord`。
汉明距离仅返回0—64的不同位数，不内置阈值或决定保留/丢弃。指纹不是加密哈希或唯一内容身份，
碰撞和近似误判可能发生；原创小改动样例通过不证明通用去重质量。文档库和索引由各自目标提供；去重决策由下述独立入口提供。

CTest包含固定词哈希/手算投票、完整uint64权重/坏输入/预算/独立逐位距离、
原创TXT/RSS与中英文管线接线、Python任意精度有符号投票参考和公共头编译合同。
`siftwing_simhash_probe`仅为测试验收入口，受限读取显式TF批次，不是生产CLI或持久化格式。

[duplicate_decision.hpp](include/siftwing/document/duplicate_decision.hpp)通过目标
`siftwing_duplicate_decision`提供`decide_duplicates(candidates, policy, limits)`，返回拥有型
`DuplicateReport`。候选显式提供`DocumentId`、`SourceIdentity`和`optional<SimHash64>`，不修改
`DocumentRecord`，不重新分析文本。调用方须保证指纹规则、分词和停用资源兼容；分析失败不能
伪装为`nullopt`。来源标识沿读取层相对路径规则校验，并沿`std::string`字典序、文件内序号排序，
每批文档身份及来源位置分别唯一；不按ID或传入顺序决定保留者。

`DuplicatePolicy`默认距离阈值3，合法范围0—64，距离等于阈值也命中。只与已保留的有指纹文档
比较，选最早命中者并记录身份/实际距离；命中者距离不一定最小。丢弃的候选不参与后续比较，
不形成传递聚类。无指纹候选单列`no_features`并跳过搜索保留集合，数值0指纹仍合法。
报告逐项给出`kept`/`duplicate`/`no_features`、来源和可选匹配，统计输入、保留、重复、无特征与
实际比较次数；保存实际策略副本。输出不借用输入，正文/元数据保持，推荐累计仍由调用方
按原选定语料显式执行，不自动改成搜索保留集合，也不在此计算搜索DF。

四正预算限制候选数、单个来源字节、总来源字节（每候选计一次）与实际指纹比较次数。
配置与完整输入先预检，距离预算在每次计算前检查；等于上限合法，任何普通失败返回
`duplicate.*`且没有部分决策或统计，分配/未知异常上传。函数无IO、共享可变状态；排序
O(n log n)，指纹比较最坏O(n²)，预算不是进程硬限，不提前分桶。结果只是近似规则命中，
未测真实去重准确率或大规模性能；存储和manifest由各自目标提供，正式离线建库已由[siftwing-indexer](docs/INDEXER.md)接通。

CTest使用手算非传递链和多命中、全部65×65阈值/距离组合、六候选720排列、空特征与预算失败，
并接线原创TXT/RSS/中英文完整管线，使用独立Python逐位字符参考及公共头编译合同。
测试probe仅消费显式身份/来源/指纹批次，不是生产CLI或磁盘格式。

```sh
cmake -S . -B build/sanitize -DCMAKE_BUILD_TYPE=Debug -DSIFTWING_ENABLE_SANITIZERS=ON
cmake --build build/sanitize --parallel
ctest --test-dir build/sanitize --output-on-failure --no-tests=error
```

## 工程原则

原始词到文档关系由 [posting.hpp](include/siftwing/index/posting.hpp) 和 `siftwing_posting`
提供：显式文档身份与正整数出现次数，受检创建并校验同词列表的严格文档次序/条数预算。
原始 TF、列表关系数与推荐累计各自保持口径；不自动排序/合并，不含权重或词位。
调用方仍须保证同词/同快照/搜索保留集合，详见 [POSTING.md](docs/POSTING.md)。
倒排构建、DF/文档统计和TF-IDF权重由下述索引目标提供；Query、候选集合与余弦排序见上文。

- 优先保证正确性与明确的所有权，避免过早优化。
- 公共合同、失败行为和线程边界须可测试。
- 已验证的工程状态与学习状态分别记录。
- 只发布可复现的构建、测试、调试和性能证据。

## 许可证

Siftwing 自身代码使用 MIT 许可证，详见 [LICENSE](LICENSE)。第三方组件保留各自许可证与
NOTICE，见[第三方依赖](docs/DEPENDENCIES.md)；分发含第三方静态库的程序须同时附带相关声明。

原始语料级关系由 [倒排索引构建](docs/INVERTED_INDEX.md) 和 `siftwing_inverted_index` 提供。
调用方显式交付搜索保留文档的完整单文档 TF，构建器拥有固定词表、有序唯一 Posting List、
DF 和文档/语料统计；该构建器不自行去重、不使用推荐累计，权重交由下述目标，候选集合与余弦排序见上文。

`siftwing_weights` 提供 [TF-IDF、L2归一化及规范字节导出](docs/WEIGHTS.md)。
它显式接管原始索引，保留整数TF/DF/N，按平滑自然对数IDF生成文档单位权重，
空文档保持零向量；输出顺序与编码固定，不依赖locale；该权重目标不执行查询或快照发布，后者由下述Writer提供。

`siftwing_snapshot_manifest` 提供[快照目录与manifest合同](docs/SNAPSHOT_FORMAT.md)：
固定六个payload文件，分开记录搜索/推荐的来源摘要、统计和版本，支持规范manifest
编解码、内存SHA-256/字节校验及完成标记。

`siftwing_snapshot_writer`提供[完整快照写入与发布](docs/SNAPSHOT_WRITER.md)：同步借用搜索
保留记录、加权索引及独立推荐词典，生成规范八文件，唯一临时目录写入/同步/关闭后重新
打开校验，原子无覆盖发布版本目录。成功结果区分同步完成与已发布但持久性未确认；
发布前失败保留自建临时目录供诊断，不覆盖已有对象。

`siftwing_snapshot_reader`提供[完整快照加载](docs/SNAPSHOT_READER.md)：显式选择正式版本，
核八文件集合、manifest/COMPLETE、六文件尺寸/SHA及记录、原TF/DF/权重、推荐字符跨文件关系，
成功后只读拥有全部数据且不保留fd。数值运行环境须复现保存的规范权重字节；
Query、候选集合与余弦排序见上文；正式离线建库已接通；本地查询见[CLI.md](docs/CLI.md)，在线显式发布见下文。

`siftwing_snapshot_manager`提供[当前快照与同步加载结果](docs/SNAPSHOT_MANAGER.md)，
在线查询按次持有共享只读Reader；候选同步完整加载的成功/失败结果独立保存，管理器自身不创建线程，只有显式发布才替换在线快照。

`siftwing_snapshot_loader`提供[后台候选加载](docs/SNAPSHOT_LOADER.md)：显式构造的专用线程
执行完整校验，单槽交接普通报告或异常，失败保持旧快照；停止后排空回收。
已有[安全发布与旧请求生命周期](docs/SNAPSHOT_PUBLICATION.md)：控制者显式发布，查询按次持有同一完整句柄，
旧查询完成后按共享寿命回收；支持回滚与同版本内容缓存隔离。服务/控制者提供显式C++入口，
正式CLI尚无reload/信号触发入口，默认线程数与启动版本行为保持。

[快照往返验收](docs/SNAPSHOT_ROUNDTRIP.md)通过真实Writer/Reader及独立Python参考，验证
固定元数据下重复构建、输入排列变化和加载后再发布的八文件一致性；另有原创TXT/RSS经
英文文本管线、SimHash去重及独立推荐累计到快照加载的完整测试链路。

## 正式离线建库

`siftwing_indexer`构建为`siftwing-indexer`：两支分别显式选择TXT/RSS或空集、分词资源和停用词，
搜索正文经SimHash去重/TF-IDF，推荐按全部交付累计；最后无覆盖发布完整快照。
输入/资源只读，已有版本不覆盖，stdout报告独立统计及来源摘要，stderr保留读取诊断。
命令示例、有限预算、退出码、已发布但持久性未确认的处理及证据边界见[INDEXER.md](docs/INDEXER.md)。
该程序不依赖Python或测试探针，BUILD_TESTING=OFF也可构建。

## V1固定黄金集成验收

[验收说明](docs/V1_TESTS.md)以原创TXT/RSS固定输入驱动正式indexer→完整快照→CLI，
通过人工抽取/词界黄金与独立数学参考检查正确结果、八文件损坏拒绝、确定性和同进程资源交接。
CTest注册v1.golden、v1.rejection、v1.lifecycle；原始fixture保持只读，生产接口与格式不变。
该验收不替代阶段工程封板、性能/业务质量评估或学习者本人的调试与修改证据。


服务默认以info启用[结构化日志](docs/LOGGING.md)，记录生命周期、连接与异常；debug另记录线程/连接代次/请求/索引关联、
查询耗时、结果数、缓存及队列阶段；不记录正文或路径。`--log-level debug --log-file run.jsonl`只创建新文件，
已有当前文件或配置范围内的归档时启动拒绝；文件输出自动使用spdlog大小轮转，默认10MiB、3份归档加当前文件。
日志目录须当前用户拥有且不允许组/其他用户写入，本文件族由实例独占管理；同步输出可能阻塞，写入/轮转失败停用该实例并让业务继续，不提供持久化或性能保证。
