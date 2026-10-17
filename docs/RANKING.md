# TF-IDF 与余弦排序

排序组件（链接别名`siftwing_ranking`）实现复用`siftwing_candidates`与`siftwing_weights`。
`rank_candidates(query,weighted_index,limits)`内部从同一个WeightedIndex的原始倒排
生成完整候选，返回`Result<RankedDocuments>`；不接收外部或其他快照生成的候选。
成功结果独立拥有`vector<RankedDocument>`，每项为强DocumentId与double分数。
发布时按分数降序、精确同分按DocId升序；不使用epsilon合并不同分数。
0、UINT64_MAX及稀疏DocId合法，不按身份数值分配数组。

这是完整排序，Query.top_k不截断。[top-k组件](TOP_K.md)在完整评分后按其合同选取。
查询与建库文本管线资源兼容由调用方保证，不从对象地址或manifest声明推断真实兼容性。

查询只保留当前索引已知的词，使用原始TF乘本索引IDF：
`IDF=1+ln((N+1)/(DF+1))`，N含索引中的空篇。按TermId递增用hypot累积L2范数，
再除以范数得到查询单位向量。重复TF改变多词查询方向，单词公共缩放会抵消。
OR忽略未知词，未知词不进入范数；AND含未知词为空；全未知也成功空。
文档直接使用同索引已有的单位权重，全部文档词均影响文档范数，包括未查询词。

每个候选在每个已知查询维度的Posting中二分身份，命中时按位置读取对应文档权重，
按TermId递增累积内积。正有限分数以`min(1,score)`夹到数学上界，成功满足`0<score<=1`。
例如单篇a:3/b:4且N=DF=1：查询a分数0.6，查询b分数0.8，查询a a a b b b b分数1；
查询a unknown的OR分数仍0.6，AND为空。

`RankingLimits`包含候选三项预算及正`max_score_pairs`；四项均须显式为正，含端点。
评分预算计完整候选数C乘已知唯一查询词数T，包括二分未命中的维度；
先以除法准入避免整数乘法溢出，不按原始重复次数或真实Posting命中数计量。
空结果没有评分组合，但仍要求正配置和正确浮点环境。检查顺序：
全正配置→FE_TONEAREST→既有候选完整准入与合并→空结果返回→C*T准入→数值→排序。

|错误context|含义|
|---|---|
|ranking.limits|四项配置含零|
|ranking.rounding|调用线程舍入模式不是FE_TONEAREST|
|candidate.*|保留候选错误的message/context，详见[CANDIDATES.md](CANDIDATES.md)|
|ranking.work|完整C*T超出评分预算|
|ranking.numeric|查询TF-IDF/范数或最终分数非正、非有限|

失败无部分输出，不修改Query或索引；分配及未知异常上传，局部容器RAII释放。
输入仅同步稳定借用，输出不保存输入借用，可独立复制、移动或修改。
修改结果后由调用者重新维持分数和排序性质。输入不能与所有者移动、赋值或析构并发。
函数无IO、共享可变缓存，不改变调用线程舍入模式。

沿已有权重的IEEE754 binary64边界，GNU/Clang目标禁用fast-math及浮点收缩。
同输入、策略、环境按固定次序确定；跨架构/libm版本不承诺逐位相同。
维度工作空间O(T)、输出及候选空间O(C)，评分成本O(C*sum(log(Lt+1)))，
完整排序O(C log(C+1))；另计既有候选、词表查找与维度排序成本。
预算不是实际二分比较次数或进程CPU/内存硬上限。

CTest注册`query.ranking`、`query.ranking_reference`、`query.ranking_snapshot`及
`query.ranking_compile_contract`。手算黄金与边界验证数值、精确顺序和拥有生命；
Python从原始TF逐文档以60位Decimal独立算IDF/平方和余弦，并比较两个新进程输出；
原创TXT/RSS及英文/原创中文词典经Writer/Reader验证真实加载后的评分；公共头验证
强身份、nodiscard和禁止外部候选。测试探针不是生产CLI，不证明搜索质量、性能或并发。

[稳定top-k](TOP_K.md)复用本完整排序并取前k条，保留全部候选与评分准入；本接口继续返回全量结果。
