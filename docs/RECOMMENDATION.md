# 候选词推荐

`siftwing_recommendation` 提供 `recommend_terms(key, dictionary, top_k, limits)`，返回
`Result<RecommendedTerms>`。公共权威为 [recommendation.h](../include/siftwing/query/recommendation.h)。
它消费[独立推荐词典](LEXICON.md)，可直接使用 [Reader](SNAPSHOT_READER.md) 的
`recommendation()`；搜索 Query、词→文档倒排和 TF-IDF 排序是独立支路。

输入是调用方按建库兼容规则准备的**单个词键**，严格 UTF-8、无 NUL、非空、无
Unicode15.1 White_Space。原大小写、标点、组合字符与 U+FEFF 保留；函数不隐式规范化、
NFC、casefold、分词或停用过滤。若建库使用 ASCII 小写规范化，可先显式调用
`normalize_utf8`；含多个词的空白输入拒绝，不能用搜索 Query 的词频表替代输入。

查询按 Unicode **标量**拆分，只对唯一字符的已知索引列表求并集、去重。重复字符仍参与
完整词距，但不重复召回列表或计关系预算。未知字符忽略，空/移空词典或无共享字符成功空。
只比较共享字符候选，不扫描其他词，因此**不保证全词典全局最近词**。

对全部候选计算精确 Levenshtein 距离：标量插入、删除、替换各 1，相等 0，字符交换需要
两步。标量不是 UTF-8 字节，不保证字素簇或组合字符等价。排序依次为距离升序、推荐
累计次数降序、UTF-8 字节序升序，返回恰 `min(top_k, 候选数)`。精确匹配保留，
没有距离阈值、不补齐；频次不是搜索 DF/N、单文档 TF 或 TF-IDF 权重。

词典 `cat:2, bat:5, rat:5, act:9, dog:100`，查询 `cat`：`dog` 无共享字符而不入候选；
`cat` 距离 0，`bat/rat` 距离 1、同频按字节序，`act` 距离 2。请求 3 项返回
`cat, bat, rat`。查询 `aa`、词典只有 `aqqqqqq:1, bb:100` 时仅返回 `aqqqqqq`，
即使未入候选的 `bb` 全局距离更小。

每项拥有 `term_id/term/distance/frequency`；`term` 独立复制原词键，在词典析构后仍有效。
`term_id` 仅属于原推荐词典/快照，不携带所有者身份，不能与搜索词 ID 混用。结果可复制、
移动、修改，修改后自行维护字段关系与排序。函数同步借用输入、词典和预算，无借用逃逸、
输入修改、IO 或共享可变缓存；调用方保持生命与稳定内容，禁止与词典移动/赋值/析构或
输入写并发。未实测并发，不以 const 推断任意线程安全。

`RecommendationLimits` 八项 `uint64` 均正、含端点，默认 0 拒绝：

| 字段 | 计量 |
|---|---|
| `max_query_bytes` | 完整输入 UTF-8 字节数 |
| `max_query_scalars` | 完整输入 Unicode 标量数 |
| `max_top_k` | 请求 k 的上限，k 位于 `[1,max_top_k]` |
| `max_character_links` | 唯一查询字符已知列表长度和，同词跨列表重复计入 |
| `max_candidates` | 并集去重后的全部候选数 |
| `max_candidate_bytes` | 全部候选词键 UTF-8 字节和 |
| `max_distance_cells` | 全部候选 `M*Li` 和，M/Li 为查询/候选标量数 |
| `max_output_bytes` | 返回前缀的拥有词文本 UTF-8 字节和 |

顺序：八正配置→k→完整输入字节→完整 UTF-8→NUL→词形→查询标量数→完整关系和→
唯一候选数→全部候选字节和→全部距离工作→排序前缀→输出字节。全部工作准入完成才
计算任何距离，小 k 或精确命中不能绕过完整候选/工作预算。空候选仍检查适用输入预算。
`UINT64_MAX` 请求 k 在配置允许时合法，不按 k 预分配，也不强制生成 k 项。

context 为 `recommend.limits/top_k/query_bytes/utf8/nul/query/query_scalars/character_links/
candidates/candidate_bytes/work/output_bytes`。失败只返回拥有诊断，没有部分建议；
分配及未知异常上传，局部 RAII 清理。UTF-8/NUL 诊断含原字节位置。

有界 set 并集、单行 DP 和完整排序：距离工作 `sum(M*Li)`，排序 `O(C log(C+1))`，另含
索引查找、关系并集和字节/标量准入；工作空间 `O(M+C+Lmax+H)`，H 为返回词字节。
距离预算仅计非边界 DP 单元，不计边界初始化、查找/并集、排序、容器或分配器开销；
不等于进程硬 CPU/内存限额。未做性能、真实 OOM、并发、跨平台或私有语料推荐质量评估。
正式 CLI 和网络接线留后续模块。

验证包括手算黄金、独立 Python 字符集合/完整距离矩阵、原创 TXT/RSS 英文中文管线
推荐累计→Writer→Reader→建议接线及公共头合同。显式行为检查在 Release 下仍执行；
这些测试证明限定输入合同，不证明语言学质量或全词典近邻召回率。
