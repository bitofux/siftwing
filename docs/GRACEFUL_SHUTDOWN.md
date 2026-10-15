# 优雅退出

正式服务的正常关闭通过 `OnlineServer::shutdown(timeout_ms)` 完成：停止接入和新任务，在
同一个绝对单调截止点前推进已在途的 completion 与输出，然后关闭连接并回收线程/fd。
`stop()`、析构、部分启动失败和运行故障保留立即丢弃网络输出的语义。接口合同见
[控制者](../include/siftwing/net/online_server.hpp)与
[所属 Sub](../include/siftwing/net/protocol_sub_reactor.hpp)。

```sh
build/release/siftwing-server --snapshot /absolute/snapshots --version 1 --shutdown-ms 1000
```

SIGINT、SIGTERM 和 `--run-ms` 到期使用新流程。`--shutdown-ms` 是规范十进制
0..86400000 毫秒，默认1000；0也建立截止点并立即截止。空闲连接不等待整个窗口。
当前仍使用进程入口信号处理标志，没有接入 signalfd。故障路径不等待正常排空。

## 准入与排空边界

`shutdown`、`run_once`、`stop`、报告与析构仍由创建线程串行调用。进入同步关闭后该控制者
不再调度 Main 接入；先关闭 WorkerPool 准入，再向所有 Sub 发布同一个 CLOCK_MONOTONIC
正绝对纳秒截止点，随后关闭/销毁 Main。必须先发布排空请求：Main 的旧 `close()` 会关闭
handoff 队列，否则 Sub 可能先沿旧合同立即退出。

pool 关闭与 `try_submit` 使用原队列同步边界；关闭前 accepted 的执行中/等待任务仍全部执行，
此后拒绝且保持提交源。正在返回的 Sub 提交回调可能得到同ID STOPPING，沿原错误输出预算。
`request_drain` 返回只说明请求已发布；报告 `drain_started` 才说明 Sub 已观察且不再读新请求。
未进入完整帧交付的请求、空闲连接与半包直接关闭，不额外分配一轮 STOPPING 响应。
已被 Main 移交但尚未注册到 Sub 的 fd 在观察排空后也关闭并计 `discarded`，不等待 worker join；
先关闭 handoff 入口再有限清空已有条目，通知端仍保持到完整发布者屏障。

每连接最多保留一个已 pending 请求。所属 Sub 继续验证完整身份并应用 completion，沿原
[输出预算](OUTPUT_BACKPRESSURE.md)发送；输出排空后 finish 并关闭，不处理缓存或新到的
下一帧。PING、业务拒绝和已有部分输出也服从同一规则。SHUT_WR 不撤销已 pending 请求。

## 截止与资源生命期

每个 Sub 复用自己的 timerfd，设为原连接期限与公共排空期限的最早值，不为连接新增 fd。
时间相等即到期；重复 `request_drain` 只收紧、不延长。原请求期限仍可更早关闭连接，网络
进展不刷新任何总窗口。同一观测点两类期限都已到期时，先裁决公共排空截止。
timerfd/时钟失败记录首因并停止服务；通知失败仍保留排空请求和有限等待后备，控制者按错误
回退立即停止。

| Sub 观测 | 行为与计数 |
|---|---|
| `drain_started` | 已进入排空，空闲/半包关闭，保留已 pending 请求 |
| `drain_responses` | 排空阶段输出通过期限检查且 finish 成功后关闭的连接数 |
| `drain_timeouts` | 公共截止时仍 pending 而关闭的连接数；正常策略事件，不记服务首因 |
| `quiescing` | 已停止提交/应用；关闭结果入口，通知 fd 仍等待完整发布者屏障 |
| `joined` | 线程已回收，连接和所属 fd 已清理 |

截止可能留下部分已发送响应；排空只证明字节交给本机内核，不能证明对端收到。客户端须按
[完整帧准入](NETWORK_CLIENT.md)判断成功。空闲、半包和缓存后续帧的关闭也可能表现为 RST。

Sub 活跃归零或截止后不再等新结果，关闭结果入口；晚结果沿 closed 拒绝，源仍由 worker
销毁。已经 accepted 的结果发布可能仍停在通知调用里，控制者必须等待全部 worker 执行、
任务析构和完整发布/通知返回，再撤 target、release 发布者并 join Sub。队列 closed/empty、
统计、网络截止或 quiescing 都不能替代这个屏障；禁止先关闭/复用通知 fd。

**排空期限不是整个 shutdown 的硬上限。** 当前不取消任务、不强杀或 detach worker；永久
阻塞的任务仍可让 join 无限等待。服务、提交器、结果队列和通知端的生命期不能为赶截止而
缩短。非法时长返回 EINVAL 并保持运行态；已停止后重复调用返回原首因，不重新启动或排空。

## 验证入口

真实注册的 `net.graceful_sub_*` 用独立黄金线字节、受控同钟和真实 timer 验证排空、慢输出、
等号、重复收紧、原请求期限、时钟/设时/通知故障和资源屏障。
`net.graceful_server_1/2` 用真实 indexer 快照、Main/Sub/worker 与真实查询结果验证执行中和
等待任务、共同绝对截止、拒绝新任务/accept、缓存帧不续读、晚到完整通知及原首因。
`net.graceful_process` 验证真实进程 SIGINT/SIGTERM/定时关闭、参数端点与最终报告。
包装只覆盖静态调用处，不证明任意调度、DSO内部故障、真实OOM、硬实时或生产性能。
