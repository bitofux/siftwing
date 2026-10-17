# 第三方依赖

当前业务归档为 `siftwing_v1` 和 `siftwing_full`。下文细粒度名称保留为链接别名，
组件分工与历史依赖说明不代表独立静态库或现行 CMake PUBLIC/PRIVATE 边；实际构建关系见[构建指南](BUILDING.md)。

当前TXT/RSS适配器、HTML辅助、共用文本规范化、英文/中文分词及停用词/词频使用 [utfcpp](https://github.com/nemtrif/utfcpp) 4.0.6（也称utf8cpp），
负责严格UTF-8验证和码点遍历。它是头文件库，许可证为BSL-1.0；
许可原文保留在 [utfcpp-LICENSE.txt](../third_party/licenses/utfcpp-LICENSE.txt)。

CMake通过FetchContent取得官方tag归档：

```text
https://codeload.github.com/nemtrif/utfcpp/tar.gz/refs/tags/v4.0.6
SHA-256: 6920a6a5d6a04b9a89b2a89af7132f8acefd46e0c2a7b190350539e9213816c0
```

首次配置需要网络，归档与源码保存在所选构建目录的`_deps/`，不安装到系统目录。
固定哈希用于检测下载字节变化；归档获取失败或哈希不匹配时配置失败，不静默换版本。
项目只使用其头文件，通过`siftwing_utf8cpp`的SYSTEM INTERFACE include声明依赖，
不执行上游安装规则或编译上游测试，也不修改上游源码。公共TXT/RSS/HTML/规范化/分词头文件不暴露第三方类型。

离线构建可以显式设置`-DFETCHCONTENT_SOURCE_DIR_UTF8CPP=/ABS/VERIFIED/utfcpp-4.0.6`。
该CMake覆盖绕过下载哈希检查，使用者须独立确认版本、完整性和许可；不能把任意本地目录
冒称已验证的4.0.6归档。组件升级需重新核对接口、许可与各适配器/规范化行为。M1.10复用该库校验/遍历UTF-8；
ASCII小写及Unicode15.1 White_Space固定规则由项目实现，不新增Unicode大小写/规范等价库。
M1.11通过`siftwing_tokenizer`的私有依赖复用utfcpp做完整输入验证；ASCII英文/数字词段规则
由项目实现，不使用locale、不新增分词库；中文适配见下文cppjieba。英文分词不隐式规范化，
不声称Unicode词边界或中文分词；升级utfcpp须同时重新验证分词编码与失败优先级。

## HTML片段抽取：Lexbor

HTML片段抽取使用 [Lexbor](https://github.com/lexbor/lexbor) **3.0.0**，承担HTML5
片段解析、实体还原及畸形标签恢复。项目使用其C接口，通过C++17 RAII管理解析器/文档；
公共头文件不暴露Lexbor类型。它不负责RSS XML读取、浏览器渲染、正文识别或网络抓取。

官方发布归档与固定校验值：

```text
https://codeload.github.com/lexbor/lexbor/tar.gz/refs/tags/v3.0.0
SHA-256: eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2
```

上游主许可证为Apache-2.0；core中部分数值转换代码附BSD-2-Clause条款，完整原文均在
[lexbor-LICENSE.txt](../third_party/licenses/lexbor-LICENSE.txt)中。原样保留上游
[lexbor-NOTICE.txt](../third_party/licenses/lexbor-NOTICE.txt)。项目自身代码继续MIT；
分发包含Lexbor的静态链接程序时须随附上述许可证、归属声明及NOTICE。
项目不修改上游源码，也不将上游代码改标为MIT。

CMake使用FetchContent下载并校验固定归档，在构建目录保存源码；按上游模块依赖只编译
`html`、`dom`、`core`、`ns`、`tag`和POSIX端口，形成`siftwing_lexbor`静态目标。
这些模块无额外下载依赖，仅链接系统`libm`。第三方代码使用C99，项目自身仍使用C++17。
接入不运行上游安装、示例、测试、CSS/渲染模块，也不系统安装库。ASan/UBSan配置同时
插桩Lexbor的C代码和项目C++代码，五类项目警告不施加到未修改的第三方源码。

首次配置需要网络；缺失、下载失败或哈希不匹配会使配置失败，不静默换库。
离线可设置`-DFETCHCONTENT_SOURCE_DIR_LEXBOR=/ABS/VERIFIED/lexbor-3.0.0`，该覆盖绕过
下载校验，使用者须独立核对归档版本、源码完整性、许可证及NOTICE。

抽取函数使用独立解析状态，不修改库的全局内存钩子。输入预算在解析前检查，token和
元素栈预算在解析中检查，DOM节点/深度在输出前检查，输出在字符串增长前检查；这不保证
整个进程的内存/CPU硬上限。HTML修复诊断保留为`HtmlText::recovered`，空正文与失败分别表示。
原创行为测试、公开黄金RSS字段集成及公共头编译合同覆盖实体、段落、非正文过滤、恢复、
拥有性和限额；升级依赖须重新验证这些规则及模块依赖树。

## RSS XML结构读取：tinyxml2

RSS适配器使用[tinyxml2](https://github.com/leethomason/tinyxml2) **11.0.0**，负责内存中
XML DOM构造、直接Text/CDATA和属性读取；HTML正文字段仍由Lexbor处理。公共RSS接口不暴露
第三方类型。官方tag归档及固定校验值：

```text
https://codeload.github.com/leethomason/tinyxml2/tar.gz/refs/tags/11.0.0
SHA-256: 5556deb5081fb246ee92afae73efd943c889cef0cafea92b0b82422d6a18f289
```

许可证为Zlib，完整原文保留在[tinyxml2-LICENSE.txt](../third_party/licenses/tinyxml2-LICENSE.txt)。
分发含该静态库的程序须保留相关许可证及归属；项目自身继续MIT，不修改或改标上游代码。
CMake FetchContent校验下载，只编译`tinyxml2.cpp`形成`siftwing_tinyxml2`静态目标，SYSTEM
include声明头路径；不运行上游安装/工具/示例/测试，没有额外下载依赖，也不系统安装。
项目C++17和禁用扩展同时适用于该目标；五类项目警告不施加到第三方实现，ASan/UBSan配置
插桩该实现。所有解析器实例按次独立，栈对象管理DOM生命周期。

离线可设置`-DFETCHCONTENT_SOURCE_DIR_TINYXML2=/ABS/VERIFIED/tinyxml2-11.0.0`，覆盖绕过
CMake下载哈希检查，使用者须独立核实源码版本、完整性和许可证。下载失败/哈希不匹配使
配置失败，不静默换版本。

库的Parse成功不能证明本项目XML支持合同：tinyxml2容许部分声明/实体/命名空间情况，
项目另做UTF-8、XML1.0字符/实体/声明、DTD禁用和命名空间检查。RSS支持范围和正文选择
由项目承担，不声称任意XML均可接入。tinyxml2内部元素深度限制500，项目XML节点/深度预算
在完整建树后检查；输入字节预算在解析前实施，但不承诺进程内存/CPU硬上限。业务不调用
LoadFile或联网，也不加载外部实体。升级须重新验证上述边界、RSS行为与许可。

## 中文分词：cppjieba与limonp

[cppjieba](https://github.com/yanyiwu/cppjieba) **5.6.7**（tag提交
`b3602bef7d1f67521a61788a74fb5801a0e62cd3`）承担中文Trie/DAG/HMM分词。
其头文件依赖[limonp](https://github.com/yanyiwu/limonp)，固定到该tag子模块提交
`9d74077dfcdf8073536c97a00bb79d7a3c3fdaba`。两库均MIT，原文分别保留于
[cppjieba许可证](../third_party/licenses/cppjieba-LICENSE.txt)和
[limonp许可证](../third_party/licenses/limonp-LICENSE.txt)。分发须保留版权及许可原文。

```text
https://codeload.github.com/yanyiwu/cppjieba/tar.gz/refs/tags/v5.6.7
SHA-256: 08322f7c2a06a88e52eb0fdbc286ec86a85abae5b2a33b1e1430dba50b71960e
https://codeload.github.com/yanyiwu/limonp/tar.gz/9d74077dfcdf8073536c97a00bb79d7a3c3fdaba
SHA-256: 2fff67ade507900ab7acb3c630b7b91232ff882f1b6240990875056befaa731b
```

CMake FetchContent核验归档，不执行上游安装/示例/测试。SYSTEM INTERFACE目标
`siftwing_cppjieba`只给适配器私有使用；公共头隔离第三方类型。没有系统安装或其他新增库。
离线覆盖为`FETCHCONTENT_SOURCE_DIR_CPPJIEBA`和`FETCHCONTENT_SOURCE_DIR_LIMONP`，
使用者需独立验证完整源码，覆盖绕过归档检查。首次联网失败/哈希不符即配置失败。
头文件算法编译进入`siftwing_cppjieba_tokenizer`，随项目C++17及ASan/UBSan插桩。

[本地内存初始化补丁](../third_party/patches/cppjieba_memory.cmake)先核对两个原始头SHA，
再复制到构建目录，新增拥有型DictTrie构造及默认HMMModel构造；原归档保持原样，
保留原路径API，不更改DAG/HMM算法。适配器只读装载一次并严格验证，绕开路径重读、
全局路径词典缓存和坏资源XCHECK终止。默认用户词权重取主词典中位数；显式频率取
log(freq/主词典总频率)。主词典与用户词典各自内部拒绝重复，用户词可覆盖主词。
升级必须核对原头哈希/构造布局/Trie指针生命周期、算法输出、初始化失败、同路径
新资源及三配置测试，不能仅改URL或绕过补丁检查。

资源由调用方显式提供绝对普通文件路径，无符号链接组件。运行时不推断构建目录、
下载路径或系统默认资源；主词典/HMM必需，可选单一用户词典。官方tag归档中的默认资源：

| 资源 | 字节 | SHA-256 |
| --- | ---: | --- |
| `dict/jieba.dict.utf8` | 5071207 | `6f7d4350e8861ef4139b2e3a6fad05430c19ae71f4b8378190edecac8aae2e6a` |
| `dict/hmm_model.utf8` | 519739 | `f17790586ac86dd048c8adffed052c4bd2b28ed0682972c1275e59040c0589a7` |
| `dict/user.dict.utf8`（示例，可选） | 36 | `24e5caba06a8a0bf707b00ff6e1c78afa4d878517dc2cf8b7a88707b980512d3` |

上述资源来自同一MIT仓库归档，项目未另行下载词典；自供资源需由使用者确认许可及版本。
资源不复制进公开测试夹具；原创测试用小词典与可手算HMM，不以库自身输出生成黄金答案。
不装载IDF/关键词/停用词资源。词边界依赖词典/模型/user/HMM开关；正式索引和查询须
使用兼容资源；indexer保存配置摘要，CLI报告实际资源SHA，调用方负责相容性。
`cppjieba_tokenization_policy_version=1`只标识适配分段规则。
文件预算不包含解析容器/Trie，token预算不包含库临时DAG/HMM分配，均非进程硬资源限。
分配与未知异常上传，未承诺上游在OOM下注入故障的强异常保证。

## 停用词与词频复用依赖

M1.13无新第三方库。`siftwing_term_frequency`通过CMake私有依赖复用
`siftwing_text_normalization`和固定utfcpp4.0.6：配置条目使用既有ASCII小写/Unicode空白
规范化；全部token/词键先严格校验UTF-8。过滤/单文档TF/推荐累计由标准库set/map及既有
受检整数运算实现，公共头不暴露utfcpp，不调用cppjieba的停用词或关键词组件。
停用词输入由调用者提供字节，许可/来源/版本属于调用者配置责任；公开测试只用原创条目，
不复制私有停用列表。升级utfcpp/normalize须复验编码优先级、配置匹配、内部空白和预算。

## 文本管线复用依赖

M1.14无新第三方库。`siftwing_text_pipeline`通过CMake公开链接共用normalize和term_frequency目标，
以Tokenizer虚接口接管调用方选择的英文或cppjieba实例，公共头不暴露第三方类型。
不自行加载分词/停用资源，不链接额外HTML/XML库，不使用cppjieba停用词/关键词模块。
已有utfcpp、cppjieba/limonp、Lexbor和tinyxml2的版本、来源、SHA及许可证记录保持；
具体分词资源与停用集合决定词项兼容性，管线对象身份不能替代manifest中的配置摘要；
摘要由正式indexer组装，不由管线推断。

## SimHash组件评估与复用依赖

M1.15评估了[yanyiwu/simhash](https://github.com/yanyiwu/simhash/tree/24625edde9bd86ea0ba4deabd57e905d30b3a894)
固定提交`24625edde9bd86ea0ba4deabd57e905d30b3a894`的README、Simhasher.hpp、jenkins.h及CMake。
README声明MIT并链接作者许可页；该提交文件树未提供独立LICENSE文件，本项目未引入或分发其代码。
默认入口另行持有完整Jieba并加载IDF/停用资源、提取topN关键词，predefined入口仍include完整Jieba并用double权重；
不适合直接保留现有完整TF的uint64精确投票、空输入与预算合同。因此采用项目小型整数实现，无新增第三方库，
不复制上游Simhasher/Jenkins源码，不把评估当作已完成依赖接入或去重准确率验证。

`siftwing_simhash`公开依赖既有`siftwing_term_frequency`，私有复用固定utfcpp4.0.6校验词键与遍历标量。
哈希使用明确的FNV-1a64字节递推与fmix64整数变换，常量、模2^64、权重/平票规则作为项目版本1合同固定；
不使用平台相关`std::hash`、课程源码或新哈希库。首次构建的已有五库下载/许可/哈希/离线失败行为保持。
升级utfcpp须重验词键编码与Unicode空白边界；改指纹规则须升版本并重新评审快照兼容性。
正式indexer将指纹策略和去重配置纳入搜索config摘要；Reader校验格式与已保存关系，
不能从摘要认证去重策略，见[快照格式](SNAPSHOT_FORMAT.md)。


## 去重决策复用依赖

M1.16无新第三方库。`siftwing_duplicate_decision`公开依赖既有`siftwing_document`和
`siftwing_simhash`，只使用DocumentId/SourceIdentity、已有汉明距离与C++17标准库容器/排序。
不加载分词/IDF/停用资源，不引入分桶、聚类或文件IO组件，不复制课程或上游去重代码。
既有五库来源/版本/归档SHA/许可证保持；升级指纹规则须同步审查去重与快照兼容，而距离
阈值不能替代分词和指纹资源的一致性。

## 文档库记录格式复用依赖

M1.17 无新第三方依赖。`siftwing_record_format` 公开依赖 `siftwing_document`，复用
既有 DocumentRecord、DocumentId、Result 与 checked 算术，使用 C++17 标准库字符串和流。
CRC-32/ISO-HDLC 采用显式逐位递推；公开 Python 测试以标准库 struct/zlib 独立核对字节，
Python 不进入生产库。既有五库的版本、来源、SHA 与许可证保持。

## 偏移表与随机读取复用依赖

M1.18 无新第三方库。`siftwing_document_store` 公开依赖 `siftwing_record_format`，使用
既有记录校验、强身份与受检算术；Linux/POSIX open/fstat/pread/close 由内部 RAII 管理。
测试的 GNU ld `--wrap=pread` 仅注入短读/EINTR/EOF/EIO，不进入生产目标。
Python struct/zlib 只用于独立格式参考，既有五依赖来源/版本/SHA/许可证保持。

## 词典与推荐字符索引复用依赖

M1.19 无新第三方库。`siftwing_lexicon` 公开依赖 `siftwing_base` 与 `siftwing_term_frequency`，
私有依赖既有固定 utf8cpp 4.0.6，严格验证并按 Unicode 标量拆分词键；有序容器、排序、
强 TermId 与受检频次总和由 C++17 标准库和既有基础模块提供。不加载新的词典资源。
Python 独立 bytes 排序/Unicode 集合仅用于参考测试，不进入生产库。五依赖来源/版本/
归档 SHA 与许可证保持，字符索引不引入编辑距离或推荐查询依赖。

## 原始 Posting 复用依赖

M1.20 无新第三方库。`siftwing_posting` 仅公开依赖 `siftwing_base`，复用强 DocumentId、
Result 和受检整数转换；生产目标不依赖词典、文本处理、浮点数学或资源加载。
M1.13 到单词关系的接线只在测试目标显式依赖 `siftwing_term_frequency`。
既有五依赖版本、来源、归档 SHA 与许可证保持。

M1.21 无新第三方库。`siftwing_inverted_index` 公开依赖 `siftwing_lexicon` 与
`siftwing_posting`，通过前者获得单文档 TF 接口，通过后者获得受检原始关系；
词键验证和编号复用 Lexicon，整数受检复用 base。共享管线、SimHash、去重仅为接线测试依赖。

M1.22 无新第三方库。`siftwing_weights` 仅公开依赖 `siftwing_inverted_index`，
使用C++标准库log1p/hypot、cfenv、to_chars及memcpy；GNU/Clang目标禁用fast-math和浮点收缩。
共享管线/SimHash/去重为原创接线测试依赖，不进入权重库的额外公开依赖。

M1.23无新第三方库。`siftwing_snapshot_manifest`仅公开依赖`siftwing_base`；
固定大小标准SHA-256无符号运算及规范文本编解码由项目实现，独立参考用Python hashlib。
文档存储、权重、推荐及文本管线仅在接线测试中显式依赖，不进入manifest生产库。

M1.24无新第三方库。`siftwing_snapshot_writer`公开依赖`siftwing_snapshot_manifest`、
`siftwing_document_store`、`siftwing_weights`，经权重目标复用独立词表/推荐接口。
三个新文本payload编码只在Writer内部实现，复用既有有效部件，不复制课程/第三方源码。
路径/fd/同步由libc/Linux openat、mkdirat、getrandom、write/pread、fsync/close及
renameat2(RENAME_NOREPLACE)完成；没有普通rename降级，也没有外部加密库。
测试的GNU ld --wrap只用于IO/提交点故障与竞争屏障，不进入生产目标。
Python struct/zlib/hashlib及数学/Unicode参考仅在测试使用；文本管线只为接线测试额外依赖。

M1.25无新第三方库。`siftwing_snapshot_reader`公开依赖`siftwing_snapshot_manifest`、
`siftwing_document_store`和`siftwing_weights`，复用既有格式验证和有效搜索/推荐构建器。
只读路径/fd由libc/Linux openat、fstat/fstatat、pread和目录枚举/close完成，无长期fd。
测试的Writer接线和GNU ld --wrap不进入Reader生产目标，独立Python生成器不调用Writer。

M1.26无新第三方库或生产依赖。快照往返测试只组合已有Writer/Reader；原创TXT/RSS管线
接线另链接TXT、RSS、英文Tokenizer、文本管线及去重目标。Python标准库与既有独立参考
仅用于测试，不进入生产接口；测试探针不构成生产indexer/CLI。

M1.27无新第三方库。`siftwing_query`仅PUBLIC依赖`siftwing_text_pipeline`，
查询词键/重复词频由已有文本阶段提供，不引入索引或快照依赖。英文与原创cppjieba资源、
TXT/RSS只用于对应真实接线测试；Python正则/Counter独立参考及探针不是生产查询入口。

M1.28无新第三方库。`siftwing_candidates` PUBLIC依赖`siftwing_query`和`siftwing_inverted_index`，
以标准库堆算法、二分查找和容器组合有效查询/索引。权重、Writer/Reader、TXT/RSS及英文/中文
资源仅为真实接线测试显式依赖，不进入候选库；Python逐文档集合参考不调用生产合并算法。

M1.29无新第三方库。`siftwing_ranking` PUBLIC依赖`siftwing_candidates`与`siftwing_weights`，
只用标准库vector、sort、lower_bound、hypot及cfenv，GNU/Clang目标禁fast-math及浮点收缩。
Writer/Reader、TXT/RSS与英文/原创中文资源仅用于真实评分接线测试；Python Decimal高精度
数学参考不进入生产依赖，不读取生产IDF/单位权重来生成期望。既有五库版本/SHA/许可证保持。

M1.30无新第三方库。`siftwing_top_k`仅PUBLIC依赖`siftwing_ranking`，复用完整排序后以
vector::erase缩容，不重复浮点运算或改变旧预算。Writer/Reader、TXT/RSS、英文/中文资源仅为
接线测试依赖，Decimal独立参考不进入生产；既有五库版本/SHA/许可证保持。

动态摘要目标`siftwing_summary`公开依赖查询模型，私有复用既有utfcpp4.0.6严格解码原正文；
不新增依赖，不把解码后的规范化匹配值用于显示文本或原文字节偏移。

关键词高亮目标`siftwing_highlight`公开依赖查询模型，私有复用既有utfcpp4.0.6严格解码原文字；
匹配区间保留原UTF-8字节，HTML实体与固定mark标签由项目自身生成。无新库、资源文件或
持久化格式；无网络、HTML解析器或locale依赖。参见[高亮合同](HIGHLIGHT.md)。

候选词推荐目标`siftwing_recommendation` PUBLIC依赖`siftwing_lexicon`、PRIVATE复用已固定
utfcpp4.0.6解码查询/候选标量；set并集、单行Levenshtein与三键排序由项目自身及标准库实现。
不依赖搜索Query、排名、快照IO或新增距离库；Writer/Reader、TXT/RSS、中英文资源仅为接线
测试依赖，独立Python全矩阵参考不进入生产。既有五库版本/SHA/许可证保持；参见[推荐合同](RECOMMENDATION.md)。

## 正式离线建库应用

`siftwing_indexer`（产物`siftwing-indexer`）仅复用上述固定版本及项目组件，未增第三方依赖。
通过CMake PRIVATE链接TXT/RSS、文本管线/中文分词、去重、SnapshotWriter和tinyxml2；
直接tinyxml2仅为准备RSS原item的身份，正式字段/编码/命名空间验证仍由RssDocumentReader完成。
生产应用不依赖Python或测试探针；Python仅在BUILD_TESTING时运行独立应用验收。
两支资源路径都由调用方显式提供，程序不隐式读取上游自带词典。详见[INDEXER.md](INDEXER.md)。

## 正式本地查询应用

`siftwing_cli`（产物`siftwing-cli`）PRIVATE链接Reader、top-k、摘要、高亮、独立推荐、
cppjieba适配与既有utfcpp目标。固定schema的有界JSON字符串/整数/有限分数输出由项目及
C++17标准库实现，不新增JSON库或第三方依赖；严格UTF-8验证复用utfcpp。应用不依赖Python
或Writer探针；它们仅在BUILD_TESTING时构造原创快照、运行真实进程与独立参考。
所有五库版本/SHA/许可证保持，资源仍由调用方显式提供，详见[CLI.md](CLI.md)。


## 内存帧 Body 准入：nlohmann/json

[帧 codec](FRAME_CODEC.md)使用 [nlohmann/json](https://github.com/nlohmann/json) **3.11.3**
的 SAX 接口验证 JSON 语法、转义及根对象，不构建 DOM，不向公共头暴露第三方类型；
严格 UTF-8 检查复用既有 utfcpp。解析深度最多64层，超出库数值表示范围拒绝；
业务字段、重复键语义和网络收发不由这个库或本模块实现。

```text
https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
SHA-256: 0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
```

归档以 FetchContent 固定哈希，不运行上游安装/测试；`siftwing_json` 用 SYSTEM INTERFACE
声明头路径，codec PRIVATE 依赖此目标。上游 MIT 许可原文保留在
[nlohmann-json-LICENSE.txt](../third_party/licenses/nlohmann-json-LICENSE.txt)，原文件亦包含
所用数值转换代码的归属许可，不改写为项目原创。
首次配置需要网络；离线可用 `-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/ABS/VERIFIED/json-3.11.3`，
该覆盖绕过下载哈希校验，必须独立确认来源/版本/完整性。升级须重验语法、数值、转义、
深度与异常合同，不能只检查编译。

## 结构化日志：spdlog与bundled fmt

[spdlog](https://github.com/gabime/spdlog) **1.15.3**（tag commit
`6fa36017cfd5731d617e1a934f0e5ea9c4445b13`）以header-only方式私有接入日志实现，
使用其bundled **fmt 11.2.0**，不依赖系统fmt或编译器扩展。两者MIT许可原文分别保留在
[spdlog-LICENSE.txt](../third_party/licenses/spdlog-LICENSE.txt)与
[fmt-LICENSE.txt](../third_party/licenses/fmt-LICENSE.txt)。分发含该实现的程序须保留适用许可。

```text
https://codeload.github.com/gabime/spdlog/tar.gz/refs/tags/v1.15.3
SHA-256: 15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67
```

CMake FetchContent固定归档与哈希，通过SYSTEM INTERFACE目标声明include；不运行上游工程、
安装、示例或测试，不修改上游源码，不注册全局logger或创建后台线程。仅日志实现暴露给上游库，
项目公共头文件无spdlog/fmt类型。同步sink复用spdlog锁/formatter/阈值，窄fd适配负责安全文件接管、
短写与SIGPIPE隔离；行为与局限见[结构化日志](LOGGING.md)。

离线可设置`-DFETCHCONTENT_SOURCE_DIR_SPDLOG=/ABS/VERIFIED/spdlog-1.15.3`，该覆盖绕过下载校验，
须独立核对版本、源码和许可。升级须复核formatter行界、异常处理、并发、故障隔离及bundled fmt许可。
