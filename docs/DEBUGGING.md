# 全项目调试回放

本页按六条端到端链路连接已有实现；先预测，再观察，最后解释故障和修改影响。作者预演或自动测试不能证明学习者已经掌握。精确接口以公共头和[组件参考](COMPONENTS.md)为准；V1 的逐字段路线继续见[V1 调试包](V1_DEBUGGING.md)。

## 准备和证据

Ubuntu 上需要 CMake、C++17 编译器、Python 3、GDB 和 strace。按[测试指南](TESTING.md)构建独立 Debug/Release，调试使用 Debug；不对生产环境附加调试器，不修改全局 ASLR/ptrace 配置。命令在仓库根执行，输出每次放全新目录。

```sh
siftwing_replay=$(mktemp -d /tmp/siftwing-replay.XXXXXXXX)
siftwing_debug=build/debug
mkdir "$siftwing_replay/snapshots"
git rev-parse HEAD > "$siftwing_replay/head.txt"
git diff --binary > "$siftwing_replay/worktree.diff"
sha256sum "$siftwing_debug/siftwing-indexer" "$siftwing_debug/siftwing-cli" \
  "$siftwing_debug/siftwing-server" > "$siftwing_replay/programs.sha256"
```

另存未跟踪候选的内容/哈希、配置、编译器、命令、退出码和原始日志，不能仅以 HEAD 或 `build_version` 证明程序来源。若工具缺失或 ptrace 被拒绝，保留诊断，不把断点已设置当命中。GDB 默认 all-stop 会暂停其他线程；业务期限继续按墙钟推进，多线程调试会改变时序。观察一般流程先关闭业务期限，期限专项直接运行已有测试；不要从调试耗时推性能。

常用 GDB 动作：`set pagination off`、`bt 6`、`info args`、`info locals`、`info threads`、`thread apply all bt 4`；用 `next` 看当前层，用 `step` 进入确实需要解释的调用。先 `list` 确认停靠点，优先查看已有局部/成员；调用 C++ getter 可能因内联或调试器调用限制失败。保存原诊断。重复断点用 `disable 编号` 或 `tbreak` 控制；观察完成继续到退出并核验程序退出码。

## 一、文档输入到版本化快照

调用链：`apps/indexer/main.cc` 的 `run → analyze`（搜索/推荐各一次）→ 文档读取、`TextPipeline::analyze` → 搜索 SimHash 去重 → `WeightedIndex::build` → 推荐独立累计 → `write_snapshot` → 临时目录同步/重开校验 → 无覆盖发布八文件。

```sh
gdb --quiet --args "$siftwing_debug/siftwing-indexer" \
  --output "$siftwing_replay/snapshots" --version 1 --build-version replay \
  --search-txt tests/fixtures/golden/txt/en-river.txt \
  --search-rss tests/fixtures/golden/rss/original.xml \
  --recommendation-txt tests/fixtures/golden/txt/en-workbench.txt \
  --recommendation-rss tests/fixtures/golden/rss/original.xml
```

```text
set pagination off
break siftwing::text::TextPipeline::analyze
break siftwing::index::WeightedIndex::build
break siftwing::snapshot::write_snapshot
run
bt 6
info args
```

先预测 `lantern` 的 TF、DF、去重保留 DocId 与推荐频次。indexer 主线程拥有临时容器；Writer 同步借用完整部件，发布后八文件由版本目录持久保存。不要在 `tf.clear()` 或移动之后观察容器来推断原值；在消费前停靠。两支来源、搜索保留集和推荐累计对象详见[V1 调试包](V1_DEBUGGING.md)。indexer 成功报告是 `key=value` 文本，业务查询才输出 JSON。

故障入口：同参数再次运行同版本，预期拒绝且原成品哈希不变；损坏实验只复制到新目录，不能改原始输入或已保存观测清单。`app.indexer_failures` 与 `snapshot.writer` 覆盖失败边界。代表性修改：在独立学习副本扩展一条重复输入，预测搜索去重与推荐累计的不同变化，再运行独立参考回归。

## 二、查询到排序、摘要与推荐

链路：CLI 参数/快照 Reader → `Query::from_text` → 候选 AND/OR → TF-IDF 余弦完整评分/稳定排序 → `rank_top_k` → 文档、摘要与高亮 → 拥有 JSON；SUGGEST 走字符候选并集、编辑距离和稳定排序，不复用搜索 DF。在线 SearchService 复用这些核心组件。

```sh
gdb --quiet --args "$siftwing_debug/siftwing-cli" SEARCH \
  --snapshot "$siftwing_replay/snapshots" --version 1 --query lantern --top-k 5
```

```text
break siftwing::snapshot::SnapshotReader::open
break siftwing::query::Query::from_text
break siftwing::query::rank_candidates
break siftwing::query::rank_top_k
break siftwing::query::make_summary
break siftwing::query::make_highlight
run
```

每次断点核栈、参数和对象身份；核心查询同步借用同一个 Reader，返回结果拥有自己的数据。SUGGEST 把启动参数换为 `SUGGEST --snapshot ... --version 1 --key lantern --top-k 5`，断点用 `siftwing::query::recommend_terms`。先预测未知词在 AND/OR 中的差别、同分按 DocId/词的顺序、`top_k` 对完整评分工作量的影响。

故障入口：以非法 `--top-k 0` 核实参数拒绝，另用 `query.ranking_reference`、`query.recommendation_reference`、`query.summary_reference` 验证数学/字节边界。修改练习：增加一组同分或 UTF-8 摘要边界的独立期望，解释为什么不能复制生产结果作为期望。

## 三、accept 到 Main、fd handoff 和 Sub 注册

按[演示](DEMO.md)准备快照，在 GDB 启动服务：

```sh
gdb --quiet --args "$siftwing_debug/siftwing-server" \
  --snapshot "$siftwing_replay/snapshots" --version 1 --port 0 --subs 2 --workers 2
```

```text
break siftwing::net::Listener::accept_into
break siftwing::net::try_handoff_and_notify
break siftwing::net::ConnectionIdentityTable::activate
run
```

若符号改名，先用 `info functions accept`、`rg -n 'accept4|try_handoff|void admit|activate' src/net` 查当前实现，不能把过时行号当合同。另一个终端使用实际 `PORT N` 运行 `build/debug/siftwing-cli --network PING --port N`。先预测 Main 与 Sub 的线程号及 fd 所有权交接，再用栈和 `info threads` 对照。

Main epoll 等 listener、停止 eventfd 和 signalfd；有限 `accept4` 得到非阻塞/CLOEXEC fd。移交成功时队列取得所有权，eventfd 只是通知，目标 Sub 取 fd、建立 Connection/身份并 ADD 到自己的 epoll。队列满仍由 Main 关闭源，不能把已成功入队但通知失败的 fd 再关一次；满队列不改投其他 Sub。

故障/回归：`net.main_reactor_failures`、`net.fd_handoff_queue_failures`、`net.multi_reactor`；最后一项用真实 socket 和包装观察核线程归属。修改练习：改变独立测试的 handoff 容量，验证拒绝、轮询游标和所有权保持；不要改固定合同来让断言通过。

## 四、增量收包到 Task、worker、Completion 和回包

沿上一服务追加符号断点，发送网络 SEARCH；请求先完整进入 TCP，再由所属 Sub 解码，不能把 TCP read 次数当帧数。

```text
break siftwing::net::decode_next
break siftwing::concurrency::SearchTask::create
break siftwing::query::SearchService::execute
break siftwing::net::try_publish_and_notify
break siftwing::net::Connection::queue_response
break siftwing::net::Connection::flush_output
```

实际链：Sub `Connection::read_request → decode_next` → 拥有 Frame → SearchTask/RoutedSearchTask → 有界 WorkerPool → SearchJob/execute（取得一次完整快照句柄）→ 拥有 RoutedCompletion → 结果队列+eventfd → 原 Sub 核身份、等待状态、请求号与期限 → 排队完整响应 → 非阻塞短写/保留后缀 → 输出排空后 `finish_request`。PING/SERVER_INFO 在 Sub 处理，不能用 PING 命中证明走了 worker。

先保存同一请求的 request_id、路由 generation、所属 reactor_id，以及三个线程停靠点。任务/结果跨线程拥有 Body，不带 Connection 指针；worker 不 send socket。结果入队成功也不代表通知成功或对端收到。陈旧结果应丢弃；Completion 拒绝或通知失败按既有首因协调停止。

故障入口：`net.frame_decoder`、`net.connection_identity_reuse`、`net.completion_wakeup_failures`、`net.single_inflight_worker`。修改练习：增加分片边界或同 slot 新 generation 的迟到结果案例；保持协议黄金字节独立。

## 五、慢客户端、背压、超时和关闭

核心字段从 `Connection` 的 `pending_`、`response_queued_`、`output_paused_` 追到 Sessions Slot 的 `awaiting_completion`、`deadline` 和身份表。一次交付至响应排空只允许一个在途请求；输出达到 high 暂停、降到 low 恢复输出原因，但 pending 原因仍在。硬限拒绝完整响应且不泄漏前缀。

```sh
gdb --quiet --args "$siftwing_debug/siftwing_output_backpressure_test" limits
```

```text
break siftwing::net::Connection::queue_response
break siftwing::net::Connection::flush_output
break siftwing::net::Connection::finish_request
run
```

查看 `this->pending_`、`this->output_paused_`、`this->output_watermarks_`；结合参数和 `next/finish` 看前后变化，不能把刚进函数的旧值当返回状态。该专项包含 GNU ld 注入/包装；真实 TCP 慢读隔离另由 `net.output_backpressure_tcp` 验证。

```sh
ctest --test-dir "$siftwing_debug" --output-on-failure --no-tests=error \
  -R '^(net\.output_backpressure_(limits|sub|tcp)|net\.online_deadlines_[12])$'
```

期限使用单调时间、timerfd 与每 slot 一个真实可删除条目；先撤身份再关闭。请求超时关闭连接，但已经拥有的 worker 任务继续；回包要重新核身份，不能按旧 fd 找新连接。GDB 停顿会影响期限，先用正常专项结果定位，再针对 `Sessions::expire/expire_due/drop` 的当前源码位置观察。修改练习：增加 high/low/hard 端点或超时迟到结果断言，解释局部关闭与整体失败的区别。

## 六、快照切换和 signalfd 优雅退出

调用链分两支：控制者提交精确候选 → Loader 专用线程完整校验 → 取完成值 → 显式 publish → 新 execute 取得新句柄；关闭则在线程创建前屏蔽终止信号 → Main signalfd/epoll → 停接入与限时网络排空 → Loader/worker join 与发布者静止 → Sub 回收。

正式 CLI 没有 reload 或信号热更新。不要给普通服务发送 SIGHUP 来验证切换。在线切换、旧请求固定旧 Reader、回滚和同版本不同内容使用已有集成驱动：

```sh
ctest --test-dir "$siftwing_debug" --output-on-failure --no-tests=error \
  -R '^(snapshot\.publication|net\.signalfd_online|v3\.shutdown_races)$'
```

交互观察使用[既有集成驱动](../tests/integration/snapshot_publication_test.py)的原创成品生成函数，在本次新目录保存输入；不依赖驱动成功后已清理的临时目录：

```sh
PYTHONPATH=tests/integration python3 -B - "$siftwing_replay/publication" <<'PY_REPLAY'
from pathlib import Path
import sys
from snapshot_publication_test import sample
from reader_reference_test import fixture, save
root = Path(sys.argv[1])
root.mkdir(mode=0o700)
valid, alias, broken = [root / name for name in ("valid", "alias", "broken")]
for directory in (valid, alias, broken):
    directory.mkdir(mode=0o700)
for version, label, identity, extra in [(100, "old", 11, False),
        (101, "new", 22, True), (0, "zero", 44, False),
        ((1 << 64) - 1, "maximum", 55, False)]:
    sample(valid, version, label, identity, extra)
save(valid, fixture([], {}, 0, 2))
sample(alias, 100, "alias", 33)
sample(broken, 101, "bad", 66)
(broken / "101" / "COMPLETE").unlink()
PY_REPLAY
gdb --quiet --args "$siftwing_debug/siftwing_snapshot_publication_test" \
  "$siftwing_replay/publication/valid" "$siftwing_replay/publication/alias" \
  "$siftwing_replay/publication/broken" "$siftwing_replay/publication/events"
```

日志父目录按 Logger 合同使用 0700，避免继承 umask 后出现 group-write 导致初始化拒绝。坏 COMPLETE 仅删除本次新生成的故障副本标记；原始夹具/正式成品不变。正常模式预期 `snapshot publication checks=...` 且退出0；信号模式沿同一启动命令在末尾另加 `signals`，日志参数换新名字。测试有自己的同步门，长时间交互停顿仍可能使有限等待失败；保留失败原件，再用正常 CTest 区分时序扰动。

符号断点：`SnapshotLoader::worker_main`、`SnapshotManager::load_candidate`、`SnapshotManager::publish_candidate`、`SearchService::execute`、`drain_termination_signalfd`、`OnlineServer::shutdown`（均加实际命名空间）。独立设置，不要求一次全部命中。

旧请求的局部 shared_ptr 延长整个只读 Reader 的生命周期；发布不等待所有旧请求，不修改磁盘版本。成功加载尚未发布时 current 保持，失败也保持；最后一个所有者决定回收线程，不承诺后台释放或 lock-free。缓存键还包含完整成品身份，版本可回滚或相同但内容不同。

GDB 信号观察保留 `handle SIGINT nostop noprint pass` 和 `handle SIGTERM nostop noprint pass`，避免调试器吞掉被测信号；只给本次 inferior PID 发信号。网络排空期限不等于整个退出硬截止，worker/Loader join 仍需完成。故障入口是坏 COMPLETE/加载异常/在途旧请求/关闭竞态；代表性修改在独立副本新增一组切换期间请求，核响应版本与正文始终一致。

## 字段演进与诊断顺序

| 字段/对象 | 为何引入 | 生产者 → 消费者与失效边界 |
|---|---|---|
| 原始正文 → token/TF → DocId/Posting → 权重 | 分开文本语义、身份与排名 | indexer 主线程生成，Writer 借用；临时容器清空后不能倒推旧值 |
| request_id | 关联连接内请求/响应 | 可信帧头 → Task/Completion；不作为跨连接身份 |
| ConnectionId/slot/generation 与 poll_token | 防 fd/slot 复用误投、过滤旧 epoll 事件 | 所属 Sub activate/retire；纯值路由必须再次查活跃身份 |
| pending_/awaiting_completion/输出暂停 | 区分业务在途和输出增长原因 | Sub 独占，低水位不能提前解除业务门禁 |
| deadline 与 timerfd | 无轮询空转地控制闲置/在途等待 | 所属 Sub 更新/删除，超时不取消已经拥有的任务 |
| current_ / SnapshotHandle / LoadCompletion | 区分加载结果和在线版本、保留旧请求 | Loader 只交候选，单控制者显式发布，execute 原子取得拥有句柄 |
| 缓存成品身份、执行版本观测 | 防回滚/同版本异内容串缓存和日志错标 | 同一次 execute 的 Reader → 缓存键、响应/日志；独立采样不能替代 |
| signalfd、drain 请求、发布者屏障 | 把异步信号变控制事件，防 fd 借用提前失效 | Main 发布停止，各 owner 完成职责；join/静止后释放通知资源 |

定位失败先保存原件，再依次区分：启动/配置 → 成品校验 → 帧/业务准入 → 队列满/通知失败 → 身份/期限 → 输出/退出。`strace -ff -o 新目录/trace -e trace=network,epoll_wait,epoll_ctl,eventfd2,timerfd_create,timerfd_settime,signalfd4,read,write,close 程序 参数` 可关联 syscall、线程和 fd；跟踪会改变时序，文件数字仍可能复用。成功的 syscall、测试或 GDB 命中都不能单独证明没有并发缺陷。

每条回放保留“预测 → 栈/字段/线程观察 → 故障首因与保护机制 → 亲自修改 → Debug/Release 回归 → 复述影响范围”。学习者原始回答应另存自己的记录，不能用本页或作者证据代填。面试事实与性能结论分别以可定位实现及[性能证据边界](PERFORMANCE.md)为准。
