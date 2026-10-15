# 离线建库程序

`siftwing-indexer` 将两项独立任务组合为一个完整快照：搜索语料经正文分析、SimHash去重、
倒排和TF-IDF/L2权重；推荐语料经正文分析、累计出现次数、推荐词典和字符索引。
最终由[Writer](SNAPSHOT_WRITER.md)无覆盖发布，[Reader](SNAPSHOT_READER.md)可完整加载。
调用顺序与实际应用入口见 [main.cc](../apps/indexer/main.cc)。

构建目标名为 `siftwing_indexer`，产物名为 `siftwing-indexer`；不依赖测试探针或Python。

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
mkdir -p /tmp/siftwing-snapshots
build/release/siftwing-indexer \
  --output /tmp/siftwing-snapshots --version 1 --build-version my-source-identity \
  --search-txt /absolute/search/article.txt --search-rss /absolute/search/feed.xml \
  --recommendation-txt /absolute/recommendation/words.txt \
  --search-stopwords /absolute/resources/search-stopwords.txt \
  --recommendation-stopwords /absolute/resources/recommendation-stopwords.txt
```

示例路径须替换为实际文件；输出父目录须已有，不从输入目录推导或自动创建。
`--build-version` 是调用方显式声明的稳定代码身份（1—128字节ASCII `[A-Za-z0-9._+-]`）；
应记录实际提交和未提交源码摘要。程序不验证该声明，不把常量标签或时间当成源码证据。
`--version` 必须为规范uint64十进制，0和UINT64_MAX合法，版本大小不代表时间次序。

两支分别用 `--search-txt/--search-rss` 和 `--recommendation-txt/--recommendation-rss`
选择文件，输入选项可重复。每支无文件时必须显式选择 `--search-empty` 或
`--recommendation-empty`，不能与该支文件混用；两支都空也生成完整八文件。
不递归扫描目录、不按扩展名猜测格式；TXT一文件一篇，RSS选择全部无命名空间原item，
不按条目筛选或网络抓取。相同文件可以分别选入两支，每支内部重复规范路径拒绝。

每支分词器默认英文，可独立指定：

```sh
build/release/siftwing-indexer \
  --output /tmp/siftwing-snapshots --version 2 --build-version my-source-identity \
  --search-txt /absolute/search/cn.txt --recommendation-rss /absolute/recommendation/cn.xml \
  --search-tokenizer cppjieba \
  --search-dictionary /absolute/resources/dict --search-model /absolute/resources/hmm \
  --search-hmm off \
  --recommendation-tokenizer cppjieba \
  --recommendation-dictionary /absolute/resources/dict --recommendation-model /absolute/resources/hmm \
  --recommendation-user-dictionary /absolute/resources/user --recommendation-hmm on
```

`tokenizer`只接纳 `english|cppjieba`。cppjieba须明确提供该支dictionary/model，
可提供user-dictionary，HMM默认on、可off；英文分词拒绝这些中文资源选项。
停用词可选，缺席表示明确空集合；提供空文件合法，空路径值拒绝。
资源语法见[README中的中文分词与词频说明](../README.md)。
程序仅分析正文，标题、URL、作者、日期保存为文档元数据，不拼入词频。
推荐支只保留TF与交付计数，整批在局部词表中累计，不逐篇复制已有词表。
搜索保留记录用于快照；丢弃记录及已消费的临时TF在后续构建前释放。

输入路径允许相对或绝对，拒绝 `..`，规范为当前工作目录下的绝对路径并剥首斜杠形成逻辑
来源名。逐路径组件只读openat、NOFOLLOW拒符号链接，fstat拒目录/FIFO/设备等非常规输入。
装载前检查大小，处理短读/EINTR，读前后检查大小、inode/device和mtime/ctime。
来源路径须在调用期间保持稳定；此检查不提供敌对并发改写的原子快照或认证。
原始文件从不写回。输入参数排列不影响规范来源顺序；改路径或换工作目录可能改变来源
名、DocId和来源摘要。DocId按来源名原字节序及原item序号从0编号，无文字位置仍占位，
保留文档不重新编号，所以ID可能有空洞。源文件必须由调用方保证建库期间不被修改。

全部warning/no_text都会输出定位诊断。warning仅接纳适配器判定可交付的完整记录，
部分抽取按既有ReadPolicy拒绝；rejected/failed或批量提前停止使整批失败，不发布半成品。
停用词、词典、模型等资源文件内容不合法属于建库前的资源加载/分析错误，退出1。
no_text没有交付文档；有正文但全部词被过滤则有空TF：搜索去重报告no_features，不进入
搜索N；推荐仍计一次显式交付，累计次数为0。搜索默认汉明距离阈值3，可用
`--search-dedup-distance 0..64`改变；只与已保留者比较、最早命中、非传递规则沿
[去重决策合同](../include/siftwing/document/duplicate_decision.hpp)。推荐不应用搜索去重；同来源在两支中的统计独立。

两支来源摘要都由实际字节产生，不能因为选择相同文件就隐式复用：

```text
SIFTWING-INDEXER-INPUTS 1
F <txt|rss> <规范来源名原字节小写hex> <完整文件字节数> <SHA256>
```

F行按来源名排序，文件唯一；空集合仅有头行。摘要含原BOM/换行/XML，RSS采用全部原item
的规则由配置描述声明，不把抽取正文摘要冒充原始输入摘要。
配置ASCII描述 `SIFTWING-INDEXER-CONFIG 1` 记录role、profile、适配/partial/body-only、
normalize/lexicon/tokenizer策略、五项可调预算中该支相关项、搜索SimHash/去重/权重或推荐
累计规则，以及停用/词典/模型/用户词典实际字节SHA和HMM开关。具体字段顺序由main.cc的
analyze固定；未提供停用词/用户词典写absent。资源路径不进入config摘要，相同资源字节
换路径不改变它；输入路径在input摘要内。发布前重新只读核对资源摘要，发现变化拒绝发布。
build-version、输出路径、SnapshotVersion、时间不进入两支来源/配置摘要。

有限预算profile1如下，含端点，单位为字节或条数。MiB=1048576，均为正预算，
不包含容器容量/分配器/DOM与第三方临时空间，不是进程硬CPU/内存限制。

| 层 | profile1预算 |
|---|---|
| 每支装载 | 每文件/资源16MiB；原输入累计128MiB；逻辑结果10000；标题+正文累计128MiB |
| 单篇读取 | 标题+正文16MiB；RSS XML节点1000000/深度128；HTML输入/输出16MiB、节点/文本片段1000000、深度128 |
| 规范化/分词 | 原文/规范化16MiB；分词输入16MiB、条数1000000、单词4096、累计词字节64MiB |
| 分析/停用词 | 分析输入/输出条数1000000、字节64MiB、单词4096、唯一词1000000/64MiB；停用原字节16MiB/唯一词1000000/单词4096/64MiB |
| 指纹/去重 | 每篇TF唯一词1000000/单词4096/64MiB/总TF1000000；来源单路径4096/累计64MiB；实际比较50000000 |
| 词表/倒排 | 词表输入10000000/512MiB，单词4096，唯一词1000000/64MiB；文档10000、单篇TF1000000、总TF100000000、每词posting10000 |
| 权重 | 文档10000/词1000000/总posting10000000 |
| 推荐 | 交付10000/唯一词1000000/单词4096/64MiB/总次数100000000；字符1000000/关系10000000 |
| 写入 | 单字段16MiB/单记录64MiB/文档10000/库512MiB/表1MiB；manifest65536/单payload512MiB/六payload总2048MiB |

`--max-input-bytes`、`--max-total-input-bytes`、`--max-records`、`--max-total-text-bytes`、
`--max-dedup-comparisons`可降低对应默认预算，不能为0或超过上述上界。装载前用单文件与
剩余累计输入预算较小值限制分配；RSS先数原item并检查剩余记录预算再分配身份。
读取预算用尽后仍有未处理来源会失败，不静默截断。两支共享这些上限值，但分别计量。
固定profile改变须升版本并更新配置描述和本文；自定义高容量配置留待后续真实需求。

仅单独 `--help` 输出帮助，成功退出0，实际写出失败退出4。其他单次选项不能重复，未知选项/值、非规范数字、
缺少值和empty混用退出2。stdout只在发布后写固定 `key=value` 报告，包含快照身份、两支
文件/读取/交付统计、搜索kept/duplicates/no_features/comparisons/N/词数/TF/posting、
推荐交付/词数/累计次数、两支来源和配置摘要、durability与system_errno；stderr保存定位诊断。
来源名在诊断中以字节hex表达，避免文件名换行破坏定位结构。

| 退出码 | 含义 |
|---|---|
| 0 | 帮助成功，或已发布且Writer确认父目录fsync/close成功 |
| 1 | 普通文件装载/读取/分析/建库/Writer发布前失败，没有本次成功发布报告 |
| 2 | 命令行参数/预算配置错误，未调用建库 |
| 3 | **已发布**，父目录同步/关闭持久性未确认，检查system_errno，不盲目重试同版本 |
| 4 | 分配/未知异常或报告写出失败；stderr明确报告故障是否发生在发布后 |

应用忽略本进程SIGPIPE，让关闭输出管道成为报告流错误；报告写出失败时已发布快照保留。
外部终止信号仍可能中断进程。Writer的Linux无覆盖rename、临时目录保留、发布前后错误
边界和durability合同完全沿用。发布成功不保证断电恢复；不会清理临时目录、删除已有
快照或更新CURRENT。索引器只负责建库；本地SEARCH/SUGGEST由[CLI](CLI.md)提供，网络查询由[在线服务](ONLINE_SEARCH.md)提供。私有业务质量评估未完成。

CTest的 `app.indexer_integration` 和 `app.indexer_failures` 用原创临时文件运行真实应用。
独立参考核对TF、整数SimHash去重、DF/IDF/L2、字符关系、原始来源摘要、八文件和CRC，
再用真实SnapshotReader重载；覆盖中英文资源、独立输入/停用/no_text/空支、参数乱序、
重复构建、0/UINT64_MAX及路径/编码/预算/已有版本/报告写出失败。不证明生产去重准确率、
推荐质量、大规模性能、真实OOM、任意并发修改、TSan或断电持久性。
