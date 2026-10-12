# V1黄金集成验收

V1验收以正式[siftwing-indexer](INDEXER.md)和[siftwing-cli](CLI.md)进程完成
TXT/RSS→共用文本管线→两支独立建库→完整八文件快照→SEARCH/SUGGEST。
`tests/fixtures/golden`为原创MIT输入；原始manifest只冻结输入字节/原RSS字段。
`tests/fixtures/v1/expected.json`另行人工冻结本验收的抽取正文、词界、保留身份，
有限原创词典/模型和停用资源不来自私有语料或上游默认词典。期望不由生产输出生成。

| CTest | 所证明的合同 |
|---|---|
| `v1.golden` | 原始记录/URL/源item/稀疏DocId；原TF/DF/N/权重；推荐独立累计；真实CLI数学余弦/Unicode标量距离、摘要原字节及转义高亮；英文/中文/两支不同规则与选择 |
| `v1.rejection` | 八文件逐一缺失/截短/追加，额外文件/子目录/链接，重新计算清单与COMPLETE的语义坏快照；两命令均完整校验，区分合法空与错误；输入拒绝前不发布正式版本 |
| `v1.lifecycle` | 相同输入路径/版本/配置/环境重排两次新进程构建八文件逐字节相同；新CLI结果确定；0/MAX版本；同进程重复加载、移动、查询、显示、推荐、资源成功/失败以及拥有结果析构后有效，fd无增长；stdout失败不改快照 |

人工黄金：RSS先占DocId0—2、空TXT占3，其余TXT占4—8；RSS第三item和空TXT无文字，
其余7篇交付。停用a/the/is/on，SimHash汉明距离阈值0使5/6重复0；英文搜索仅保留0/1/4，
中文有限词典/HMM关闭时另保留7/8。推荐仍累计自己所选的全部交付，空TF计交付，
不将搜索DF/N当推荐次数。混合测试只选两份TXT为中文推荐输入，英文搜索仍选择全部输入；
显式选出的中文建议可能在英文搜索库合法无匹配。

Python独立解码SWDR/CRC、计算TF/DF/数学IDF/L2/余弦、二维Unicode标量距离、窗口/HTML
区间并集，整数关系精确比较、浮点值有限误差比较；精确排序以真实核心稳定合同为准。
测试专用C++生命周期探针复用生产核心，在同一进程运行32轮，以`/proc/self/fd`逐段
观察Reader成功装载完成后、失败Result仍存活时及所有者析构后的fd基线；不以进程退出
替代及时关闭的证据。拥有显示/分词结果跨越Reader/管线/词典生命，sanitizer另检查受测
内存路径。此探针不是生产入口，Python仅是测试依赖。

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^v1\.'
```

完整构建/测试还应分别运行Release和独立Debug `-DSIFTWING_ENABLE_SANITIZERS=ON`。
所有行为检查在Release仍执行，不依赖C/C++ assert。三组各自使用临时目录、120秒CTest
期限，原始fixture和资源只读；Writer约定保留的失败临时目录不属于应自动删除的资源。
库的加载稳定、查询/建库资源兼容仍是调用方前置条件；SHA不是真实性认证。

本验收不证明大规模性能、真实OOM、任意并发写、业务质量、跨libm/CPU逐位确定性、
跨平台、TSan或断电恢复。fd观测不是heap泄漏证明；一次sanitizer通过不能穷尽所有资源
失败。V1阶段工程封板与学习掌握仍有各自门禁，不能从这三个测试推断整个阶段已封板。
