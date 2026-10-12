# 原始倒排索引构建

公共合同权威为 [inverted_index.hpp](../include/siftwing/index/inverted_index.hpp)。
`siftwing_inverted_index` 从调用方显式选择的 `vector<IndexDocument>` 构建内存索引；
每项拥有强 DocumentId 和完整 M1.13 单文档 TF。构建器拒重复 ID，不代替 M1.16 去重，
不从推荐累计表构造单文档关系。M1.16 的 no_features 默认跳过搜索保留集；调用方若额外
显式选入空文档，本构建器仍接受并计入 N。这是输入选择，未改变去重报告的合同。

成功索引独立拥有固定搜索 Lexicon、按 TermId 对齐的 Posting List、按 DocumentId
递增的文档统计，以及语料统计。词键精确并集使用 M1.19 UTF-8 字节排序编号：不同输入
排列得到相同结果；不同词集合可能改变 ID，不能跨词典/快照解释相同数值。严格 UTF-8、
无 NUL/空词/Unicode White_Space，不重新规范化、分词或停用过滤。

例如 doc7 的 apple/banana 次数是 2/1，doc42 的 apple/pear 次数是 3/1，另显式选入
一个空文档，则 N=3、总次数=7、总关系数=4；apple 列表为 (doc7,2)、(doc42,3)，DF=2。
TF 是单文档出现次数，DF 是含该词的唯一文档数，累计出现次数 5 不能替代 DF。
文档统计记录 total_tokens/distinct_terms，后者是该篇贡献的关系数；语料 total_postings
等于所有 DF 之和。空批次成功空索引，已知词列表非空，未知词 ID 返回 nullptr/nullopt，
未知文档 ID 返回 nullptr。DocId 0/UINT64_MAX、TF 1/UINT64_MAX 合法，所有加法受检。

九项预算必须全正：Lexicon 的 input_terms/input_bytes/term_bytes/distinct_terms/
vocabulary_bytes，以及 documents/document_tokens/total_tokens/postings_per_term。
跨文档相同词键逐次计输入，唯一输出只计一次；input_terms 同时限制总关系数。
等于上限合法，预算不含输入装载、容器容量、分配器和排序开销，不是硬内存/CPU限制。

检查顺序：全正配置、文档数、只读文档排序/重复 ID；按 ID 检查每篇正 TF、受检总和
及统计一致、单篇次数、全库受检次数；每篇关系数与词键字节预算；Lexicon 全部词键
验证及唯一输出预算；最后每词列表插入前条数预算。多重错误按这一顺序返回首个诊断。
context 为 index.limits/documents/doc_id/tf/overflow/document_tokens/total_tokens/
postings_per_term/term；词表失败沿用 lexicon.*，内部不可能的查找缺失也显式失败。
失败没有部分索引，不修改输入；分配及未知异常上传，局部对象由 RAII 销毁。

InvertedIndex 不可复制，无公开默认构造；noexcept 移动/赋值使源明确空且统计归零，
自移动保持。lexicon/documents/postings/document 为只读借用，只允许左值所有者；移动、
赋值或析构使借用失效。document_frequency/statistics 返回独立值。同步只读查询无共享
可变缓存，所有者生命及读写同步由调用者负责，未实测并发。

统计合法不能证明真实原文、去重历史、资源兼容或快照身份。当前不计算 IDF/TF-IDF、
权重与归一化，不实现 AND/OR、排序、推荐候选、持久化、发布或 CLI。文档指针排序
O(D log D)，词表排序 O(P log P)，关系查找 O(P log V)，文档查询 O(log D)，按 TermId
查询列表/DF O(1)；字符串比较的字节成本另计。额外空间随 D、P、词表字节增长。
