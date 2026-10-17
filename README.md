# Siftwing

Siftwing 是 C++17/Linux 单机文档搜索服务：将 TXT/RSS XML 建成完整只读快照，提供本地查询和基于 TCP 的搜索、候选词推荐。

离线核心使用规范化、英文或 cppjieba 分词、停用词、SimHash 去重、TF-IDF 余弦排序和稳定 top-k；在线服务使用 Main-Sub Reactor、固定 WorkerPool、有界队列及 eventfd 回投。已有配置、结构化日志、指标，以及显式 C++ 控制接口下的后台加载、安全发布和旧请求快照保持。

## 构建与验证

学习时可先构建 `siftwing_v1_apps`，再构建 `siftwing_full_apps`。V1 形成独立静态库，
full 只编译 V2/V3 实现并复用 V1；命令、库边界和自建程序示例见[按学习范围构建](docs/BUILDING.md)。

活动平台为 Ubuntu，要求 CMake 3.20+、C++17 编译器；启用测试还需 Python 3.8+。首次配置通过 CMake 获取固定版本并核对归档哈希；[依赖与许可证](docs/DEPENDENCIES.md)说明来源及离线配置。公开构建与演示只使用仓库内原创夹具。

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel 2
ctest --test-dir build/debug --output-on-failure --no-tests=error

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 2
ctest --test-dir build/release --output-on-failure --no-tests=error
```

三个正式程序为 `siftwing-indexer`、`siftwing-server` 和 `siftwing-cli`，另有只链接 V1 的学习入口 `siftwing-v1-cli`。下面给出最小本地查询；完整网络四命令、正常关闭、预期结果和错误排查见[可复跑演示](docs/DEMO.md)。每次使用全新输出目录，原输入保持只读。

```sh
siftwing_demo=$(mktemp -d /tmp/siftwing-demo.XXXXXXXX)
build/release/siftwing-indexer \
  --output "$siftwing_demo" --version 1 --build-version public-demo \
  --search-txt tests/fixtures/golden/txt/en-river.txt \
  --search-rss tests/fixtures/golden/rss/original.xml \
  --recommendation-txt tests/fixtures/golden/txt/en-workbench.txt \
  --recommendation-rss tests/fixtures/golden/rss/original.xml
build/release/siftwing-cli SEARCH --snapshot "$siftwing_demo" --version 1 \
  --query lantern --top-k 5
```

`public-demo` 是快照的声明标签，不能证明源码或程序身份；复现实验另保存提交、未提交差异、程序和输入 SHA。[测试指南](docs/TESTING.md)给出完整回归、消毒器专项和失败证据方法。

## 文档入口

| 目的 | 入口 |
|---|---|
| 理解整体数据流、线程归属与设计取舍 | [DESIGN](docs/DESIGN.md) |
| 从公开输入建库、查询并运行服务 | [DEMO](docs/DEMO.md)、[索引器](docs/INDEXER.md)、[本地 CLI](docs/CLI.md)、[在线服务](docs/ONLINE_SEARCH.md) |
| 实现或排查客户端 | [协议版本 1](docs/PROTOCOL.md)、[网络客户端](docs/NETWORK_CLIENT.md) |
| 理解和校验磁盘成品 | [快照格式](docs/SNAPSHOT_FORMAT.md)、[Writer](docs/SNAPSHOT_WRITER.md)、[Reader](docs/SNAPSHOT_READER.md) |
| 调整启动行为与观察运行 | [配置](docs/SERVER_CONFIG.md)、[日志](docs/LOGGING.md)、[指标](docs/METRICS.md)、[信号](docs/SIGNALS.md) |
| 理解加载、发布、缓存与旧请求 | [后台加载](docs/SNAPSHOT_LOADER.md)、[安全发布](docs/SNAPSHOT_PUBLICATION.md)、[版本缓存](docs/VERSIONED_LRU.md) |
| 复跑测试、故障、性能与自动检查 | [TESTING](docs/TESTING.md)、[可靠性](docs/V3_RELIABILITY.md)、[性能](docs/PERFORMANCE.md)、[CI](docs/CI.md) |
| 沿六条调用链观察字段、线程和故障 | [全项目调试回放](docs/DEBUGGING.md) |
| 查细粒度接口与历史基线 | [组件参考](docs/COMPONENTS.md)、[V1](docs/V1.md)、[V2](docs/V2.md)、[V1 调试入口](docs/V1_DEBUGGING.md) |

## 当前边界

- 固定线程、单连接一个在途请求；Sub 独占连接，worker 只计算并发布拥有结果。缓存和空闲/请求期限默认关闭，监听默认 `127.0.0.1`。
- 加载与发布是两个显式步骤；正式 `siftwing-server` 没有 reload 命令或信号入口。SIGINT/SIGTERM 走 signalfd 正常关闭，网络排空期限不取消任务、不限制全部线程 join 时长。
- 支持 TXT 与 RSS XML，HTML 抽取只服务 RSS 正文。没有 JSONL 导入、独立 HTML/PDF/DOCX/Excel/Markdown 适配、HTTP、TLS、DNS、Redis、BM25、mmap 或分布式服务。
- 大小和工作预算不等于进程 RSS/CPU 硬上限；合成语料与 loopback 测量不代表生产容量或真实语料质量。快照摘要用于检测损坏和区分内容，不能认证发布者。
- [质量门禁](docs/CI.md)保留 155 份字节未变的历史格式债务；修改文件须整体符合 clang-format 18。有限 tidy 与消毒器执行只证明其受测范围。具体提交的 CI 结果以对应 Actions 运行记录为准。

## 许可证

项目自身代码与原创夹具使用 MIT，见 [LICENSE](LICENSE) 和[夹具许可](tests/fixtures/golden/LICENSE)。分发第三方代码或含静态依赖的程序时，须同时保留[第三方许可证与声明](docs/DEPENDENCIES.md)。
