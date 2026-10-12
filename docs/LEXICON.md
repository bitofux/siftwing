# 固定词表与推荐字符索引

`siftwing_lexicon` 提供两个独立的拥有对象：`Lexicon` 将词映射到局部 `TermId`，
`RecommendationDictionary` 将 M1.13 的推荐累计表变成词频词典与 Unicode 字符索引。
公共接口以 [lexicon.h](../include/siftwing/index/lexicon.h) 为准。

`lexicon_policy_version=1`：输入词按 UTF-8 **字节序**唯一排序，从 0 连续编号；重复与输入
顺序不影响相同词集合的编号。空输入成功为空词表。编号仅属固定词表/快照；插入排序靠前的
新词可能改变所有后续编号，不提供永久 ID 注册或运行时增删。搜索词典与推荐词典的同数值
ID 不保证同词；TermId 类型也不携带词典身份，调用方必须使用匹配的所有者。

词键严格 UTF-8、无 NUL、非空、无 Unicode15.1 White_Space（集合同 normalize）。
原大小写、标点、组合字符、U+FEFF 和其他合法标量保留；不隐式规范化、分词、NFC/NFKC、
case folding 或二次停用过滤。调用方先用共享文本管线产生词项，并保证索引/查询资源兼容。
`find(string_view)` 是精确字节查找，未知/空/非法查询键只返回 nullopt，不承担查询解析。
`term(TermId)` 对已知 ID 借用词键，未知 ID（包括 UINT64_MAX）返回 nullopt；不把最大值当
无效哨兵。完整词表可按 ID 枚举；本模块不计算搜索 TF/DF/N、Posting 或权重。

LexiconLimits 五项均正，等于上限合法：输入条数、输入字节和、单词字节、唯一词数、唯一
词键字节和。重复计入输入预算，唯一输出只计一次。先正配置/输入条数和字节预算，完整
UTF-8/NUL，再词形，最后排序/唯一输出预算；拥有字符串复制发生于输出预算通过后。
排序借用的临时 vector 开销及容器/分配器开销不在字节预算内，也不提供进程硬资源限制。

推荐构建接纳完整 `RecommendationFrequencies`：正词频，受检 uint64 总和等于 total_tokens；
documents=0 必须空表/总数零，正 documents 允许空表（全过滤或空文档）。documents 是
显式交付次数，不是搜索去重后的 N；重复交付可增加推荐频次，不默认使用搜索保留集合。
输入集合、规范化、分词与停用配置由上游明确选择；不验证不可从累计表证明的历史。

RecommendationDictionaryLimits 包含上述五预算及四正预算：交付次数、总次数、不同标量
数、唯一 `(标量,TermId)` 关系数。所有九项配置优先，统计受检，字符/关系预算在对应插入
前检查，失败无部分对象。频次 vector 与本推荐 Lexicon 按 ID 对齐；total_tokens/documents
保留输入统计，不把频次当 DF，也不凭统计相同证明与搜索词典兼容。

字符索引为按 `char32_t` 数值排序的 map，每个列表是递增、唯一的本推荐局部 TermId。
词内重复字符只贡献一条关系，词频也不重复关系；中文和 emoji 以 Unicode 标量拆分，
不是 UTF-8 字节、字素簇或完整 Unicode 文本等价归一化。例：词表 `a,b,中中,😀a` 对应
ID `0,1,2,3`，字符 `a`→`[0,3]`、`中`→`[2]`、`😀`→`[3]`。
这个索引不保存 DocumentId，不是词→文档倒排；候选并集/去重、编辑距离及 top-k 排序留给
推荐查询模块。词典与字符索引提供确定性内存产物/枚举，持久化规范见[快照格式](SNAPSHOT_FORMAT.md)，由[Writer](SNAPSHOT_WRITER.md)/[Reader](SNAPSHOT_READER.md)发布和加载。

两个对象不可复制、允许 noexcept 移动与赋值；移动源清为空词表（推荐频次/字符索引及
统计也清空），自移动保持。输入同步借用，成功后对象独立拥有；Result 失败只有诊断。
`terms/term/lexicon/frequencies/characters` 的借用只允许左值，有效至所有者移动/赋值/析构；
调用方不得并发修改/移动/析构所有者。值返回的 ID/次数不延长所有者寿命。只读查询无共享
可变缓存，未实测并发或 TSan。无文件 IO、资源加载或自动生产接线，分配/未知异常上传。

普通诊断 context：`lexicon.limits/input_terms/input_bytes/term_bytes/utf8/nul/term/
distinct_terms/vocabulary_bytes`；推荐统计与字符预算为 `dictionary.limits/documents/table/
overflow/total_tokens/characters/character_links`，词键失败沿用 `lexicon.*`。UTF-8/NUL 诊断含
输入词序号与 0 基字节位置；字节序和枚举确定性不证明分词质量或推荐效果。
