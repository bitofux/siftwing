# 本地查询程序

纯 V1 学习入口为 `siftwing-v1-cli`，本页本地参数也适用于该程序；它不编译网络模式。
完整 `siftwing-cli` 的本地与网络行为保持，库边界和构建命令见[构建指南](BUILDING.md)。

本文描述本地快照模式；显式`--network`的四命令与传输合同见[网络客户端](NETWORK_CLIENT.md)。

`siftwing-cli` 从正式完整快照执行独立的 SEARCH 和 SUGGEST。CMake目标为`siftwing_cli`，
产物名为`siftwing-cli`，生产程序不依赖Python或测试探针。

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
build/release/siftwing-cli SEARCH --snapshot /absolute/snapshots --version 1 \
  --query 'red blue' --mode and --top-k 10
build/release/siftwing-cli SUGGEST --snapshot /absolute/snapshots --version 1 \
  --key 'cat' --top-k 5
```

路径/版本须替换为由[indexer](INDEXER.md)或Writer实际发布的快照。`--snapshot`是已有父目录，
`--version`为规范uint64十进制，0与UINT64_MAX合法；只打开该版本，不扫描最新版本或解释
CURRENT。两种命令都由[Reader](SNAPSHOT_READER.md)完整校验八文件与两支语义；即使只做
推荐，搜索部件损坏也拒绝。目录/内容须在装载期间稳定，逐组件拒符号链接和`..`。
全内存加载，不写回/修理/删除快照，不提供网络、交互循环或热切换。

SEARCH复用[Query](QUERY.md)和[稳定top-k](TOP_K.md)。`--query`为原UTF-8文字，
`--mode and|or`默认or，`--top-k`默认10。AND/OR文字本身是普通词；重复词保留TF，未知
词沿AND/OR规则处理，合法无匹配成功返回空数组。空、无词或全停用输入为业务失败，
不冒充无匹配。k大于命中数不补齐，也不绕过全部候选与评分工作准入。

SEARCH分词默认english，停用词可通过`--stopwords 文件`提供。中文/混合语料须显式选择：

```sh
build/release/siftwing-cli SEARCH --snapshot /absolute/snapshots --version 1 \
  --query '中国北京 RED' --tokenizer cppjieba \
  --dictionary /absolute/resources/dict --model /absolute/resources/hmm \
  --user-dictionary /absolute/resources/user --hmm on
```

dictionary/model为cppjieba必需资源，user-dictionary可选、hmm默认on；英文拒绝中文资源
选项。资源格式沿既有分词/停用合同。调用方须保证与**搜索建库**的规则和资源相容；
快照只保存配置摘要，没有可恢复配置清单，CLI不从摘要推断资源，也不声称自动认证兼容。
JSON分别报告快照该支来源声明和实际查询资源SHA，方便调用方核对。相同资源内容换路径
不会改变结果。资源只读逐组件NOFOLLOW/普通文件检查，装载前限额、短读/EINTR及前后
metadata核验，初始化前SHA与输出前重读SHA比较；不是敌对并发写入的原子快照。

命中身份按原DocId向同一Reader回查，不能当数组下标。每条结果含原标题/可选URL、
有限余弦score及[摘要](SUMMARY.md)/[高亮](HIGHLIGHT.md)。`--summary-window`默认160
Unicode标量，允许1—4096；`--summary-context`默认40，允许0且须小于窗口。改变窗口
时应同时给出合法前文值，例如`--summary-window 4 --summary-context 0`。
显示扫描全部返回正文，短窗口/早命中不省略完整工作准入。摘要保留原文字节，
`source_byte_offset`相对完整正文；高亮spans的`byte_offset/byte_length`相对摘要原文字节。
HTML仅用于正文文本位置，标题/URL仍是原文本，使用者在呈现处另按上下文转义。

SUGGEST的`--key`为与**推荐建库**相容的单个词键，默认字面保持；可显式
`--normalize-key`使用既有ASCII小写/Unicode空白折叠与trim，再检查单词形状。
两种方式都不重新分词或过滤停用词，不接纳SEARCH的query/mode/资源/摘要选项。
它调用[推荐核心](RECOMMENDATION.md)，只比较共享字符候选，按Unicode标量编辑距离升序、
推荐累计频次降序、UTF-8字节序升序，保留精确匹配。频次不是DF；不保证全词典最近词。

先建议后搜索由使用者明确选词：运行SUGGEST，查看results，选择其中的term作为下一次
SEARCH的query。也可以直接SEARCH。CLI不自动替换查询或选第一条；推荐词可能不在独立
搜索词表中，选词后仍可成功返回空搜索结果。

stdout只交付一个版本1的JSON对象和LF。身份`document_id/term_id/snapshot_version`用
规范十进制字符串，保留完整uint64；其他计数为整数，score为有限数值，按max_digits10
且不依赖locale输出。搜索terms按词键排序、包含重复次数；results保持核心的稳定次序。
共同字段为schema_version、command、snapshot_version、build_version、top_k、该支
input_sha256和snapshot_config_sha256。搜索另有mode/query_config/terms，推荐另有
key/normalized。资源缺席为`"absent"`，hmm对英文为null；URL缺席为null，已提供空串仍为空串。

字符串保留严格UTF-8，JSON双引号/反斜杠及ASCII控制字节转义，包括NUL；原文不规范化。
Reader的记录编码合同允许原始元数据字节。若命中标题/URL不是UTF-8，本次显示失败，
不静默替换或改写快照；正文另由完整显示校验和摘要合同拒绝非法UTF-8/NUL。
SUGGEST不显示这些无关原字段。先有界构造完整JSON，再一次调用流写出；预输出普通失败
没有部分JSON，真正IO失败可能已经写出前缀，不能将截短内容当成功对象。

profile1固定有限预算如下，含端点，MiB=1048576，预算不是进程硬CPU/内存限制。

| 阶段 | 上限 |
|---|---|
| Reader记录/文件 | 字段16MiB、记录64MiB、文档10000、库512MiB、偏移表1MiB；manifest65536、每payload512MiB、六payload总2048MiB |
| Reader搜索/推荐 | 词表输入10000000/512MiB、单词4096、唯一词1000000/64MiB；文档10000、单篇TF1000000、总TF100000000、每词posting10000；推荐字符1000000/关系10000000 |
| 查询文本 | 原文/规范化65536；token65536、单词4096、词字节4MiB；保留token65536/4MiB、唯一词4096/4MiB |
| 搜索 | k100、唯一查询词4096、全部posting10000000、候选10000、候选×已知维度50000000 |
| 显示 | 全部返回正文标量×全部查询键标量之和50000000；每正文16MiB、查询键4096/4MiB、窗口4096标量；高亮原文16384/区间16384/输出262144字节 |
| 推荐 | key4096字节/4096标量、k100、关系10000000、候选1000000/64MiB、全部距离单元50000000、输出词文本4MiB |
| 资源/JSON | 每资源16MiB；停用词1000000/单键4096/词库64MiB；完整JSON含LF16MiB |

六项入口可降低相应预算，须为正且不超过上表：`--max-query-bytes`、`--max-top-k`、
`--max-work`、`--max-output-bytes`、`--max-snapshot-bytes`、`--max-resource-bytes`。
query-bytes同时限查询原文/规范化，SUGGEST取它与4096的较小值；work分别限制完整评分、
全部结果显示及推荐距离工作，高亮/摘要自身仍检查完整工作。snapshot-bytes降低各payload
及文档库上限，六payload总上限保持。max-top-k降低后默认请求10也须合法，不自动缩小。
本次最多100条显示，各函数临时容器、第三方资源解析与Reader全内存载荷不等于上述硬限制。

| 退出码 | 含义 |
|---|---|
| 0 | 单独--help成功，或完整查询/推荐报告成功；允许空results |
| 1 | 资源/Reader/查询/显示/实际预算失败；stderr保留具体context |
| 2 | 命令、选项、规范数字或正预算配置错误；未调用Reader |
| 3 | stdout写出/flush失败，可能存在JSON前缀；输入快照保持 |
| 4 | 分配或未知异常；RAII释放本次拥有资源 |

只接纳单独`--help`；其他选项均不能重复。应用忽略本进程SIGPIPE以处理关闭管道，
不改变全局设置；外部终止信号仍可中断。实际输入/查询校验在Reader成功之后执行。

CTest注册`app.cli_integration`与`app.cli_failures`。真实indexer→CLI覆盖原创英文中文、
TXT/RSS、独立输入/资源、稀疏身份、空集与显式选词流程；Python独立计算TF-IDF、二维
标量距离、窗口和高亮，额外Writer探针仅补表示合同夹具。验证不证明业务质量、大规模
性能、真实OOM、任意并发写、TSan、跨平台或断电恢复。
