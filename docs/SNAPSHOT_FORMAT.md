# 快照目录与 manifest v1

本模块已提供 `siftwing_snapshot_manifest` 的内存 manifest 编解码、SHA-256、payload
字节校验和完成标记。公共 API 合同见 [manifest.h](../include/siftwing/snapshot/manifest.h)。
以下目录/payload布局是本版本冻结的合同。[Writer](SNAPSHOT_WRITER.md)已实现规范编码、
完整集合落盘及原子无覆盖发布；[Reader](SNAPSHOT_READER.md)已实现指定正式版本的完整加载和跨文件校验。

调用方配置输出父目录，每个正式快照使用不透明 `SnapshotVersion` 的规范无前导零
十进制叶子目录。0及UINT64_MAX合法，数值大小不代表时间次序；发布者负责唯一性。
Writer明确临时目录命名与原子发布，不从原始语料目录派生或写回输入。
正式目录恰包含以下八个普通文件，不含其他文件、子目录或符号链接；本模块不访问目录，
因此内存校验成功尚不能证明这一文件系统条件。

| 文件 | 格式 | 含义 |
|---|---|---|
| manifest.txt | SIFTWING-SNAPSHOT 1 | 完整清单、身份、两支统计和来源声明 |
| COMPLETE | SIFTWING-COMPLETE 1 | 绑定完整manifest字节的完成标记 |
| documents.bin | M1.17 SWDR v1 | 搜索保留文档记录的连续拼接，可空 |
| offsets.bin | M1.18 SWDI v1 | 原有偏移表，精确覆盖documents.bin |
| search-lexicon.txt | SIFTWING-LEXICON 1 | 独立搜索词表与局部TermId |
| search-weights.txt | M1.22 SIFTWING-WEIGHTS 1，策略1 | 原TF/DF/N、文档统计、IDF和L2权重 |
| recommendation-dictionary.txt | SIFTWING-RECOMMENDATION 1 | 独立推荐词表、累计出现次数 |
| recommendation-characters.txt | SIFTWING-CHARACTERS 1 | Unicode标量到推荐TermId的唯一关系 |

两支都必须有完整产物。空搜索文档库是零字节；其偏移表仍有32字节头。
其余空集合仍有头和S统计行，不能用零字节文件或省略清单表示空集合。
SnapshotVersion、manifest格式、payload格式、权重策略和build_version分别维护。

manifest是ASCII、单个空格分字段、LF结尾，恰十行，严格按以下顺序：

```text
SIFTWING-SNAPSHOT 1
I <SnapshotVersion> <build_version>
S <N> <搜索词数> <总TF> <总关系数> 1 <搜索input_sha256> <搜索config_sha256>
R <推荐交付数> <推荐词数> <累计次数> <字符数> <字符关系数> <推荐input_sha256> <推荐config_sha256>
F documents.bin 1 <bytes> <sha256>
F offsets.bin 1 <bytes> <sha256>
F search-lexicon.txt 1 <bytes> <sha256>
F search-weights.txt 1 <bytes> <sha256>
F recommendation-dictionary.txt 1 <bytes> <sha256>
F recommendation-characters.txt 1 <bytes> <sha256>
```

整数是完整uint64规范十进制：0为`0`，正数无前导零，无正号/负号。
build_version是1—128字节ASCII `[A-Za-z0-9._+-]`，标识产生这些文件的代码构建版本；
建议提交身份加明确未提交源码摘要，不能把时间戳、绝对目录或机器名混入稳定身份。
摘要必须是64位小写hex SHA-256；可表示全零等合法值，词法合法不证明来源真实。

搜索N来自调用方显式搜索保留集，含显式空篇；总关系数为全部DF之和，满足
词数<=关系数<=总TF，零词/关系/总TF等价，N=0要求空统计，关系数<=N*词数。
推荐交付数与累计次数独立于搜索去重；词数<=累计次数，零词等价于零累计/字符/字符关系，
正词要求正字符、关系数>=词数且字符数<=关系数；交付数0只能有空累计。
documents.bin在N=0时必须0字节，其他情况下至少88*N；offsets.bin必须32+24*N。
乘加与六文件总长度不能溢出uint64。其他四文件长度必须正。
这些只是必要元数据约束，不能替代文件解码、实际TF之和、DF、词表对应等语义验证。

三项正预算是manifest完整输入/输出字节、单payload字节、六payload受检总字节。
均含端点；文件总预算不包含manifest/COMPLETE，不追回输入装载或容器/分配器开销。
编码逐append检查，解码在字段字符串/对象分配前检查完整输入长度；固定行/字段数限制
解析结构，不按任何攻击者声明的计数扩张容器。输入不被改写，成功输出独立拥有。
Manifest本身可修改，复制独立，默认移动后有效未指定；每次消费重新validate。

输入摘要必须来自该支显式选择的输入及内容的规范描述。建议描述为ASCII：
`SIFTWING-INPUTS 1\n`后按稳定相对来源键的UTF-8字节序列出
`F <来源键字节小写hex> <完整源字节数> <sha256>\n`，每来源键唯一；RSS选择item时
描述须额外明确记录选择规则/ordinal，不把抽取正文摘要冒充原始输入摘要。
配置摘要来自调用方明确版本化的规范配置字节，记录输入适配/规范化/分词规则、组件版本、
词典/HMM/用户词典/停用词实际字节摘要，以及该支去重或累计规则、输出算法和影响输出的预算。
两支可共享资源，却不能默认共享输入/config摘要。摘要描述生成由调用方负责，当前helper
只计算给定内存字节SHA-256，不自动读原始语料、资源或验证规范描述。

新冻结的三个payload文本都使用相同的规范整数、单空格/LF与词键原UTF-8字节小写hex。
词键遵守M1.19严格UTF-8、无空/NUL/Unicode White_Space、按无符号UTF-8字节排序；
TermId为连续0基位置。搜索词表与search-weights的T条目词键/ID/数量须完全一致。

```text
SIFTWING-LEXICON 1
S <搜索词数>
T <TermId> <词键hex>
```

```text
SIFTWING-RECOMMENDATION 1
S <交付数> <累计次数> <推荐词数>
T <TermId> <词键hex> <正累计次数>
```

```text
SIFTWING-CHARACTERS 1
S <字符数> <关系数>
C <Unicode标量十进制> <TermId> [后续TermId...]
```

C行按标量数值严格递增，标量非NUL/代理项，<=0x10FFFF；每行至少一个递增唯一推荐
TermId，每词重复标量只贡献一个关系；字符索引须恰好对应推荐词键含有的标量集合。
推荐频次之和、字符关系之和与manifest/R/S行须一致。文档库/偏移表/权重的文档ID及N
须一致，所有posting必须属于该库；文档与词ID仍是快照局部身份。
manifest组件只冻结上述payload格式；Writer内部已实现新payload规范编码，
Reader负责解码正式目录并验证跨文件语义及当前数值运行环境的规范字节兼容性。

COMPLETE恰为`SIFTWING-COMPLETE 1 <SnapshotVersion> <manifest字节sha256>\n`。
必须先成功parse完整规范manifest才生成/接受标记；逐字节严格比较，拒尾随/旧身份/旧摘要。
manifest不列自己与COMPLETE，避免自校验循环；Writer在payload全部验证后写这两项，
Reader须同时核对marker、清单文件字节和跨文件语义。单独标记成功不表示文件已经完成或发布。

诊断context为manifest.limits/metadata/statistics/version/length/overflow/file_bytes/
total_bytes/bytes/syntax/hash_length/role/checksum/completion。验证配置优先，其次来源声明、
统计、编码尺寸、按角色顺序的格式/摘要/尺寸/单项/总预算；parse先配置、输入长度、
固定词法/版本，再语义。未知必需字段/版本拒绝，不静默忽略或自动迁移。
普通失败不返回部分对象/字节，分配及未知异常上传。无共享可变缓存，输入生命/读写同步
由调用方负责，未实测并发；内存函数无文件IO、链接处理、fsync或发布。

SHA-256实现使用固定栈状态处理内存完整字节，长度须<=UINT64_MAX/8；独立参考使用
Python hashlib。校验检测意外损坏，不提供签名、认证或敌对写者下的不可变保障；重新计算
所有哈希的改写者仍能伪造合法清单。新文件角色或破坏性编码变化需显式版本迁移，
同环境权重数值的确定性与M1.22一致，不扩大为跨libm/CPU版本逐位保证。
