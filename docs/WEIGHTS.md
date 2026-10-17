# TF-IDF、归一化与确定性导出

公共合同权威为 [weighted_index.hpp](../include/siftwing/index/weighted_index.hpp)。
权重组件（链接别名`siftwing_weights`）实现复用 `siftwing_inverted_index`，没有新第三方库。

策略版本 1：TF 是原始正出现次数，N 是显式搜索集合的文档数，DF 是含词文档数。
IDF = 1 + ln((N+1)/(DF+1))，使用自然对数；实现使用等价的
`1 + log1p((N-DF)/(double(DF)+1))`，完整 uint64 统计不作整数加一。
`smoothed_idf` 要求 1 <= DF <= N，拒绝零和不可能的统计；N=0 时没有已知词，构建不调用它。
这与推荐累计词频或交付数无关。原计数仍为完整整数，double 转换可能舍入，仅用于权重。

每个关系先计算 w=double(TF)*IDF；按固定 TermId 次序用 hypot 累计每篇 L2 范数，
再将该篇每个分量除以范数。原始次数与相对 TF 在同篇 L2 下只差一个抵消的公共缩放，
这里选择原次数。平滑并加一让共有词的 IDF 为 1，避免全共有词导致零向量。
非空篇归一化权重正、有限、<=1，平方和在浮点容差内为 1。空篇保留统计、范数为 +0，
不生成零权重占位，不做除零。空批次和全空文档成功。

例如一篇 a:3/b:4 的单文档语料，两个 IDF 均为 1，范数 5，单位分量为 0.6/0.8。
另加一篇空文档会改变 N 和 IDF，但不会产生空文档 Posting；不同词 DF 不同时，
平滑变化可以改变权重方向，不能把“加空篇不影响所有权重”当成合同。

`WeightedIndex::build(InvertedIndex,WeightLimits)` 按值接管完整有效原始索引，调用方必须
显式移动，成功或失败/异常都不归还已交付输入。这个所有权选择避免复制完整词表/关系；
调用方需要原始关系时从成功对象的 `raw_index()` 借用。原始索引、IDF 数组、归一化
权重数组和范数数组由同一对象拥有。每词权重数组与原始列表等长、逐位置对应，位置不是
DocId。无旧 API 迁移，不改原 Posting 或搜索/推荐/去重合同。

类不可复制，无公开默认构造，noexcept 移动/赋值使源为空原始索引和空数组，自移动保持。
raw_index/weights 借用只允许左值，移动/赋值/析构失效；IDF/document_norm 按值返回。
未知词/文档为 nullptr/nullopt，已知空篇为范数0。所有者生命和读写同步由调用方负责，
同步只读无共享可变缓存，未实测并发。TermId 不携带词典/快照身份，跨词表误用不能检测。

三正构建预算：documents/terms/postings，含端点。检查顺序为全正配置、FE_TONEAREST/
浮点位布局、文档数、词数、总关系数，最后权重数值。诊断 context 为 weights.limits/
rounding/platform/documents/terms/postings/numeric；单独 IDF 先统计再舍入检查，额外
weights.statistics。分配/未知异常上传，普通失败无部分 WeightedIndex，局部输入 RAII 释放。
预算不追回已有输入装载，不含容器容量/分配器开销，不是硬内存/CPU上限。

要求 IEEE754 binary64 和与 uint64 兼容的位布局，构建检查 1.0 位值。数值运算要求
FE_TONEAREST，库目标禁用 fast-math 和浮点表达式收缩。固定输入/策略/本次 Ubuntu
工具与 libm 环境可重复；不承诺跨架构、不同 libm 或编译器版本逐位一致。
独立数学参考采用相对容差 1e-12，空零值精确检查。不用容差删除关系或合并身份。

序列化版本 1 返回拥有 string；是当前权重部件的规范文本，不是完整业务快照。
所有行用单个 ASCII 空格分隔，LF 结尾，无空行：

```text
SIFTWING-WEIGHTS <导出版本> <策略版本>
S <N> <总token次数> <总关系数>
D <DocId> <文档token次数> <文档唯一词数> <未归一化范数位值>
T <TermId> <词键字节hex> <DF> <IDF位值>
P <TermId> <DocId> <原始TF> <归一化权重位值>
```

先 S，再 DocId 递增的全部 D，再 TermId 递增的 T，每个 T 后接 DocId 递增的 P。
整数为无前导零十进制，词键为原 UTF-8 字节的小写 hex，浮点为 16 位小写 hex 表示的
binary64 数值位模式，+0 为 0000000000000000；不写 C++ 对象布局或主机原始字节序。
整数 to_chars 与显式位编码不依赖 locale，导出阶段不执行浮点运算，可在不同舍入模式读。
独立正 max_bytes 限完整文本长度，含端点，每次 append 前检查；失败返回
weights.serialization_limits/serialization_bytes，无部分字符串，分配/未知异常上传。

构建复杂度 O(D + V + P log D)，额外权重空间 O(P+D+V)，原始输入由移动接管，不复制。
导出复杂度随字节长度，成功拥有输出。本组件只负责内存权重与规范导出；
[Writer](SNAPSHOT_WRITER.md)/[Reader](SNAPSHOT_READER.md)组合持久化，[排序](RANKING.md)组合候选与查询向量，
[稳定top-k](TOP_K.md)限制返回条数。正式CLI由其模块承担。
