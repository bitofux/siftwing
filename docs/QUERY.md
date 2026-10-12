# 查询模型

目标 `siftwing_query` 提供 `siftwing::query::Query::from_text`，PUBLIC依赖
`siftwing_text_pipeline`。查询与建库共用调用方提供的初始化管线；不读文件、不加载快照，
不对查询再实现一套规范化或分词规则。调用方负责资源和停用配置与建库相容。

`QueryOptions` 默认 `QueryMode::any`（OR）、`top_k=10`，可显式选择
`QueryMode::all`（AND）。模式是独立选项，文字中的AND/OR是普通词，不是运算符。
`QueryLimits` 显式提供文本三阶段预算和正 `max_top_k`；请求条数必须位于
`[1,max_top_k]`，等于上限合法，默认10也不自动压缩到更小预算。不表示实际命中数量。

检查顺序：`max_top_k` 正配置→合法模式→请求条数→既有管线完整配置与文字处理→非空词频。
前3项失败的context分别为 `query.limits`、`query.mode`、`query.top_k`；
管线错误保留原message/context。管线仍允许空文本结果，Query将空原文、无词、
全停用转换为 `query.empty`，无部分查询，分配及未知异常上传。

成功查询拥有过滤后 `DocumentTermFrequencies`，唯一词键按UTF-8字节序存储，
次数为正，`total_tokens` 包含重复。例如 `THE Red red BLUE`，停用词为 `the` 时，
查询TF是 `blue:1, red:2`，总数3。重复词不增加AND约束，但为后续查询向量保留词频。
不保存原文、词序/词位、TermId或输入/管线引用；不存在短语、布尔表达式或隐式索引访问。

未知词保持合法查询。[候选模块](CANDIDATES.md)将AND中任一未知词解释为空结果，OR忽略未知词，
全部未知仍为空结果；这与过滤后没有有效词的业务错误不同。Query模型冻结这些边界，候选合并由上述独立目标实现；
查询向量与完整评分由[排序模块](RANKING.md)提供；[稳定top-k](TOP_K.md)消费查询请求条数；
[动态摘要](SUMMARY.md)使用词键定位原文窗口；[高亮](HIGHLIGHT.md)生成转义HTML与原字节区间。
推荐查询由后续模块实现。

Query不可默认构造/赋值，可复制构造独立拥有状态；noexcept移动完整转交并将源TF
清空、总数归零，源选项保持。移空源不得作为有效业务查询，`empty()`可识别它。
`frequencies()`只允许const左值借用，移动/析构使原借用失效，不延长所有者生命。
只读无可变缓存，所有者与输入的并发生命由调用方同步，未进行并发专项验收。
预算不是内存/CPU硬上限；UTF-8字节与Unicode标量数不能混用。

真实CTest注册 `query.model`、`query.reference`、`query.ingestion`、
`query.compile_contract`。独立ASCII正则/Counter参考验证固定种子查询和两次新进程；
原创TXT/RSS及英文/原创中文词典接线验证同一管线产生相同词键和次数；
错误、预算、复制/移动、管线生命和临时getter拒绝分别验收。
测试探针不是生产CLI，不证明搜索质量、已加载快照配置真实或候选/排序正确。
