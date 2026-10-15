# V1 调试学习包

本页把[V1基线](V1.md)变成两条可操作的调试路线。使用 Ubuntu、GDB、strace 和公开
原创夹具；先写预测，再操作和解释。作者预演、自动测试和阅读本页都不能代替学习者
亲自完成调试、故障实验及代表性修改。生产合同仍以各组件公共头和公开说明为准。

## 准备隔离环境

从仓库根目录运行，保持同一 shell 的变量。需要 CMake、C++17 编译器、Python 3、
GDB 和 strace；缺少工具时先记录缺项。下面不使用私有语料，不覆盖已有构建或快照。
`mktemp` 目录没有自动清理，后续保存证据时记录实际路径、源码提交和未提交差异。

```sh
v1_lab=$(mktemp -d /tmp/siftwing-debug.XXXXXXXX)
v1_build="$v1_lab/build-debug"
cmake -S . -B "$v1_build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$v1_build" --parallel
mkdir "$v1_lab/input" "$v1_lab/snapshots" "$v1_lab/gdb-snapshots" "$v1_lab/broken"
cp tests/fixtures/golden/txt/en-river.txt "$v1_lab/input/search.txt"
cp tests/fixtures/golden/txt/en-workbench.txt "$v1_lab/input/recommendation.txt"
cp tests/fixtures/golden/rss/original.xml "$v1_lab/input/feed.xml"
cp tests/fixtures/v1/stopwords.txt "$v1_lab/input/stopwords.txt"
"$v1_build/siftwing-indexer" \
  --output "$v1_lab/snapshots" --version 1 --build-version debug-exercise \
  --search-txt "$v1_lab/input/search.txt" --search-rss "$v1_lab/input/feed.xml" \
  --recommendation-txt "$v1_lab/input/recommendation.txt" \
  --recommendation-rss "$v1_lab/input/feed.xml" \
  --search-stopwords "$v1_lab/input/stopwords.txt" \
  --recommendation-stopwords "$v1_lab/input/stopwords.txt" --search-dedup-distance 0
"$v1_build/siftwing-cli" SEARCH --snapshot "$v1_lab/snapshots" --version 1 \
  --query 'lantern' --stopwords "$v1_lab/input/stopwords.txt" --top-k 5
"$v1_build/siftwing-cli" SUGGEST --snapshot "$v1_lab/snapshots" --version 1 \
  --key 'lantern' --top-k 5
```

`debug-exercise` 是来源声明，不是已验证的源码身份。建库成功应有 `durability=synced`；
SEARCH/SUGGEST 成功退出 0，stdout 为一个完整 JSON。每个版本是父目录下的十进制
目录；第二次建同版本会拒绝。资源须在调用期间稳定，查询停用规则与搜索建库保持相容。
首次配置可能取得固定依赖，离线复用方法见[依赖说明](DEPENDENCIES.md)。

## 路线一：文档到快照

开始前读 TXT/RSS 原文，预测每支交付篇数、空篇、`lantern` 的单篇 TF、搜索 DF 与
推荐累计频次。分别记录去重前后统计；不要用词出现总次数回答“有多少篇包含该词”。
阅读 [INDEXER](INDEXER.md)、[倒排](INVERTED_INDEX.md) 和[权重](WEIGHTS.md)。

```sh
gdb --quiet --args "$v1_build/siftwing-indexer" \
  --output "$v1_lab/gdb-snapshots" --version 1 --build-version debug-exercise \
  --search-txt "$v1_lab/input/search.txt" --search-rss "$v1_lab/input/feed.xml" \
  --recommendation-txt "$v1_lab/input/recommendation.txt" \
  --recommendation-rss "$v1_lab/input/feed.xml" \
  --search-stopwords "$v1_lab/input/stopwords.txt" \
  --recommendation-stopwords "$v1_lab/input/stopwords.txt" --search-dedup-distance 0
```

在 GDB 中逐条输入：

```text
set pagination off
set print pretty on
break apps/indexer/main.cc:452
break siftwing::text::TextPipeline::analyze
break apps/indexer/main.cc:594
break apps/indexer/main.cc:621
break apps/indexer/main.cc:624
break apps/indexer/main.cc:636
break siftwing::snapshot::write_snapshot
run
bt 5
print role
continue
bt 5
print input
```

行号是该 V1 实现的入口定位提示；先 `list` 确认所在语句，后续源码移动时用符号和
`rg -n 'Analysis analyze|std::vector<document::DuplicateCandidate>|tf.clear|recommendation.frequencies.clear|const auto &raw|write_snapshot' apps/indexer/main.cc` 重新定位。
`analyze` 会分别进入 search/recommendation，管线会多次命中。用 `info breakpoints` 看编号，
观察一次后 `disable 编号`，避免无意义重复；依次在下表的批次、TF和累计停靠点保存观察，再继续至 `raw`。

| 停靠点 | 观察动作 | 必须解释的关系 |
|---|---|---|
| `analyze` | `print role`、`print branch.sources`、`bt 5` | 同一管线为何有两支输入，谁负责分配来源与 DocId |
| `TextPipeline::analyze` | `print input`，`next` 跟随规范化/分词/过滤，`bt 5` | 原文、规范化字节、token 与 TF 不是同一个对象 |
| `run` 第594行，两支analyze均已返回 | `print search.frequencies`、`print recommendation.frequencies` | 此时两支原始TF尚未移动或清空，搜索去重与推荐累计使用各自输入 |
| 第621行，执行 `tf.clear()` 之前 | `print duplicates.counts`、`print tf`、`print documents` | TF与正文只包含搜索保留集，原DocId不重编号；`search.frequencies`此时已清空 |
| 第624行，执行 `recommendation.frequencies.clear()` 之前 | `print recommendation.frequencies`、`print accumulated` | 推荐仍按其全部交付TF累计，未复用搜索保留集 |
| 下一行已有 `raw` 引用 | `print raw`、`print documents`，观察原 DocId 和词表/Posting | DF 是列表篇数，TF 是关系上的次数，空篇对 N 的处理由合同决定 |
| `write_snapshot` | `info args`、`bt 5`，继续至进程结束 | Writer 同步借用局部部件；完整编码/校验后无覆盖发布，不负责重新分词 |

到第636行时，`search.frequencies`、`tf`和`recommendation.frequencies`已清空，`accumulated.terms`也已释放；这是已消费临时数据的正常生命周期，不能在该处还原之前的TF或累计词表。需要复看时，以新输出版本重新运行并在清空前停靠。

GDB 调用 C++ getter 可能受内联或调试器函数调用限制；优先 `print` 已有局部值和
`info locals`，不要把 `Cannot evaluate function` 当程序失败。停止观察后继续至正常退出，
检查八文件与建库报告。重复调试要换输出父目录或未用版本，不能删除原快照来掩盖重试。

## 路线二：查询到结果

先预测 query 的有效词/TF、候选与顺序、原 DocId、摘要和高亮跨度，再启动：

```sh
gdb --quiet --args "$v1_build/siftwing-cli" SEARCH \
  --snapshot "$v1_lab/snapshots" --version 1 --query 'lantern' \
  --stopwords "$v1_lab/input/stopwords.txt" --top-k 5
```

```text
set pagination off
break siftwing::snapshot::SnapshotReader::open
break siftwing::query::Query::from_text
break siftwing::query::rank_top_k
break apps/cli/main.cc:475
break siftwing::query::make_summary
break siftwing::query::make_highlight
run
bt 5
info args
continue
```

每次停靠先 `bt 5`/`info args` 再继续；在 `search` 第475行观察 `print q`、`print ranked`。
Reader 内部也会重建并核查索引，不能把其调用误认成 Query 评分；Query 未进入前，
完整 Reader 必须已成功返回。排名是拥有结果；`reader.document(hit.document_id)` 按身份
查原记录，DocId 不能用作 vector 下标。摘要借用完整正文，生成拥有的原文窗口；高亮
再借用摘要，先合并原文命中区间再 HTML 转义。

在 `make_summary`/`make_highlight` 中查看参数及调用栈，记录原文/摘要的字节长度与
Unicode 标量数。`source_byte_offset` 相对全文，spans 相对摘要原文；HTML 实体展开后的
下标不能用于原文切片。参照[摘要](SUMMARY.md)/[高亮](HIGHLIGHT.md)。完成观察后
继续至退出，核 JSON。用 `--query 'lantern lantern'` 另跑一次解释重复词 TF；用 SUGGEST
另跑观察推荐词典，选择词之后再显式 SEARCH，CLI 不自动替换查询。

## 系统观察与故障实验

只读查询的系统调用回放：

```sh
strace -o "$v1_lab/search.strace" -e trace=openat,read,pread64,close,write \
  "$v1_build/siftwing-cli" SEARCH --snapshot "$v1_lab/snapshots" --version 1 \
  --query 'lantern' --stopwords "$v1_lab/input/stopwords.txt" --top-k 5 \
  > "$v1_lab/search.json"
```

定位版本目录、COMPLETE/manifest/六 payload 的打开、读取和关闭，再区分 stdout 的
`write`。动态加载器也会打开文件，不把它们算成业务快照。该观察只能说明本次实际
系统调用，不能单凭 fd 关闭断言没有 heap 泄漏。

先写每个实验的预期退出码、stderr 和是否产生成功 stdout，再执行并立即用 `echo $?`
记录真实退出码。失败后用原命令回归，核原输入/完整快照 SHA 不变。

```sh
# 预算拒绝：查询有7字节，只允许1字节。
"$v1_build/siftwing-cli" SEARCH --snapshot "$v1_lab/snapshots" --version 1 \
  --query 'lantern' --max-query-bytes 1
echo $?

# 只破坏新复制的版本，完整快照保持只读。
cp -R "$v1_lab/snapshots/1" "$v1_lab/broken/1"
python3 - "$v1_lab/broken/1/COMPLETE" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
path.write_bytes(b"invalid-complete\n")
PY
"$v1_build/siftwing-cli" SEARCH --snapshot "$v1_lab/broken" --version 1 --query 'lantern'
echo $?
"$v1_build/siftwing-cli" SUGGEST --snapshot "$v1_lab/broken" --version 1 --key 'lantern'
echo $?
```

这两类失败应退出 1，拒绝交付成功 JSON。两命令都完整校验快照；推荐也不能绕过坏
COMPLETE。用 GDB 的 `catch throw` 查看预算或 Reader 错误如何经应用 `take` 转成
`Failure`，再由 main 写 stderr；不要假定每个 Result 失败都会在组件内抛异常。
降低预算不是实际分配失败，也不是 Linux OOM kill；本练习不制造真实 OOM。
Writer 提交后同步不确定及 stdout 写出失败具有不同错误合同，详见 INDEXER/CLI。

## 代表性 C++ 修改与验收

学习者在仓库外新建源码副本，只复制公开 tracked 文件；不要复制私有资料、活动修改或
整个 `.git`。工作树须先确认公开修改是否已提交；下面复制的是 HEAD 的公开树：

```sh
mkdir "$v1_lab/source-copy"
git archive HEAD | tar -x -C "$v1_lab/source-copy"
```

任务：在副本的 `tests/unit/highlight_test.cc` 添加一个独立手算用例，同时覆盖前置
HTML 特殊字符、相邻中文与 emoji 命中、合并区间及未命中后缀。自行选择 query/body，
在运行前写出 UTF-8 原文字节表、预期跨度和完整安全 HTML；可用已有 `sample` helper，
禁止用 `make_highlight` 输出生成期望值。不要改生产匹配/转义合同。

```sh
cmake -S "$v1_lab/source-copy" -B "$v1_lab/modify-debug" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$v1_lab/modify-debug" --target siftwing_highlight_test --parallel
ctest --test-dir "$v1_lab/modify-debug" -R '^query.highlight$' --output-on-failure --no-tests=error
cmake -S "$v1_lab/source-copy" -B "$v1_lab/modify-release" -DCMAKE_BUILD_TYPE=Release
cmake --build "$v1_lab/modify-release" --target siftwing_highlight_test --parallel
ctest --test-dir "$v1_lab/modify-release" -R '^query.highlight$' --output-on-failure --no-tests=error
```

验收需要本人修改 diff、原始手算、两配置真实非空 CTest 和解释。将副本中的一个预期
`byte_offset` 临时改错 1，证明 Debug/Release 都能失败；保留失败证据，再恢复正确期望并
复测。该故障检查测试是否有效，不等于覆盖全部高亮算法。活动仓库保持原字节。

另将推荐TXT副本复制到一个不同的新路径，以该路径加一份相同内容的推荐交付（同一路径重复参数会被拒绝），先预测推荐累计与搜索 DF 是否变化，再以新版本建库
观察；输入实验补充统计理解，不能替代上述 C++ 修改。所有本人预测、观察、错误和
修正按时间保留。学习验收还须解释两支统计、Reader 借用、发布提交点、错误清理，以及
将同一查询搬到网络消费者时哪些生命周期前提必须继续满足；网络消费者已由[在线服务](ONLINE_SEARCH.md)提供，本页V1练习不代替其并发与生命周期验证。
