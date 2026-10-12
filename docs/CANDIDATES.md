# 候选文档集合

`siftwing_candidates` PUBLIC依赖`siftwing_query`和`siftwing_inverted_index`。
`collect_candidates(query,index,limits)`同步借用已验证的查询与当前有效索引，返回
`Result<CandidateDocuments>`；成功值独立拥有`vector<DocumentId>`，发布时严格递增且唯一。
调用方可复制、移动或修改结果，修改后不保证原有序性。原始列表仍归索引拥有，函数不改输入。

词键从当前索引的Lexicon查找局部TermId，随后取得同索引的Posting；不接受外部可变裸列表，
不混用搜索/推荐词典身份，也不将DocId数值作数组下标。0、UINT64_MAX及稀疏身份均合法。
建库与Query文本管线资源兼容仍由调用方负责，候选层不二次规范化、不检验manifest声明。

AND（all）对不同词键求文档交集，任一未知词使结果为空；OR（any）求文档并集并忽略未知词。
全部未知或空/移空索引均成功返回空集合。重复TF不增加布尔约束，Query.top_k不截断候选。
例如a列表={0,5}，b列表={5,UINT64_MAX}，查询a a b：AND={5}，OR={0,5,UINT64_MAX}。
工厂Query不为空；移空Query返回`candidate.query`，不会被解释成AND空约束匹配所有文档。

三项正`CandidateLimits`须显式配置，0不表示无限，等于上限合法：

|预算|计量|
|---|---|
|max_query_terms|Query唯一词键数，含未知词，不按重复次数计|
|max_postings|所有已知词完整列表的长度总和，合并前准入|
|max_candidates|最终唯一命中文档数，不限制AND可能更大的中间集合|

检查顺序为全部正配置→非空Query→唯一词数→按词键次序准入全部已知列表→合并输出。
AND有未知词也先核已知列表的完整准入，结果为空不能绕过输入预算。
失败context为`candidate.limits/query/terms/postings/documents`，无部分结果；
分配及未知异常上传，局部容器RAII释放。预算控制条目数量，不包含容器容量/分配器开销，
不等于实际访问/比较次数或CPU/内存硬上限。

AND从最短列表逐身份二分探测其余列表，只存最终交集；OR以每列表一个游标的最小堆产生
递增身份流，相邻相等只发布一次。T为已知词数，P为完整准入条数，C为输出数：
OR时间O(P log(T+1))、工作游标O(T)；AND为最短列表长度乘其余列表二分成本，
工作列表指针O(T)，两者输出O(C)。索引词表查找另计Query词键二分成本。
函数无IO、打分、排序或共享可变缓存，输入必须稳定且不能与所有者移动/赋值/析构并发。
独立[排序模块](RANKING.md)内部组合本函数与同索引权重，候选接口及预算语义保持。

真实CTest为`query.candidates`、`query.candidates_reference`、`query.candidates_snapshot`、
`query.candidates_compile_contract`。手写集合核未知/重复/端点/预算/拥有生命；Python固定种子
逐文档测试含词关系，独立于堆/二分算法；原创TXT/RSS及英文/原创中文词典经Writer/Reader
证明已加载原始倒排与Query共享词键。测试探针不是生产CLI，不证明搜索质量、排序或并发能力。
