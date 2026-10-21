# 优雅退出

正式服务的正常关闭通过 `OnlineServer::shutdown(timeout_ms)` 完成：停止接入和新任务，在
同一个绝对单调截止点前推进已在途的 completion 与输出，然后关闭连接并回收线程/fd。
`stop()`、析构、部分启动失败和运行故障保留立即丢弃网络输出的语义。接口合同见
[控制者](../include/siftwing/net/online_server.hpp)与
[连接配置](../include/siftwing/net/protocol_connection_config.hpp)。

```sh
build/release/siftwing-server --snapshot /absolute/snapshots --version 1 --shutdown-ms 1000
```

SIGINT、SIGTERM 和 `--run-ms` 到期使用新流程。`--shutdown-ms` 是规范十进制
0..86400000 毫秒，默认1000；0也建立截止点并立即截止。空闲连接不等待整个窗口。
[终止信号](SIGNALS.md)在线程创建前屏蔽，由唯一Reactor的signalfd/epoll观察；故障不等待正常排空。

## 准入与排空边界

`shutdown`、`run_once`、`stop`、报告与析构仍由创建线程串行调用。进入同步关闭后该控制者
先关闭WorkerPool准入，设置一个CLOCK_MONOTONIC正绝对纳秒截止点，注销并关闭Listener。
随后在同一调用线程继续逐轮处理Core、结果队列、控制fd和连接I/O；不能sleep等待已取消的Sub。

pool 关闭与 `try_submit` 使用原队列同步边界；关闭前 accepted 的执行中/等待任务仍全部执行，
此后拒绝且保持提交源；所有网络提交也在当前线程，进入shutdown前的提交调用已经返回。
报告`drain_started`表示Core已观察截止且不再读取新请求。
未进入完整帧交付的请求、空闲连接与半包直接关闭，不额外分配一轮 STOPPING 响应。
生产不存在handoff队列或未注册移交残留；旧独立Sub及fd交接专属测试已退役，适用排空断言由连接核心测试继续覆盖。

每连接最多保留一个已pending请求。唯一Reactor继续验证完整身份并应用completion，沿原
[输出预算](OUTPUT_BACKPRESSURE.md)发送；输出排空后 finish 并关闭，不处理缓存或新到的
下一帧。PING、业务拒绝和已有部分输出也服从同一规则。SHUT_WR 不撤销已 pending 请求。

## 截止与资源生命期

唯一Reactor复用自己的timerfd，设为原连接期限与公共排空期限的最早值，不为连接新增fd。
时间相等即到期；同步shutdown不重入、不延长期限。原请求期限仍可更早关闭连接，网络
进展不刷新任何总窗口。同一观测点两类期限都已到期时，先裁决公共排空截止。
timerfd/时钟失败记录首因并停止服务；通知失败仍保留排空请求和有限等待后备，控制者按错误
回退立即停止。

| 单Reactor观测 | 行为与计数 |
|---|---|
| `drain_started` | 已进入排空，空闲/半包关闭，保留已 pending 请求 |
| `drain_responses` | 排空阶段输出通过期限检查且 finish 成功后关闭的连接数 |
| `drain_timeouts` | 公共截止时仍 pending 而关闭的连接数；正常策略事件，不记服务首因 |
| `quiescing` | 已停止提交/应用；关闭结果入口，通知 fd 仍等待完整发布者屏障 |
| `stopped` | worker完整join、连接及控制fd已清理；没有额外I/O线程需要join |

截止可能留下部分已发送响应；排空只证明字节交给本机内核，不能证明对端收到。客户端须按
[完整帧准入](NETWORK_CLIENT.md)判断成功。空闲、半包和缓存后续帧的关闭也可能表现为 RST。

活跃归零或截止后不再等新结果，关闭结果入口；晚结果沿closed拒绝，源仍由worker
销毁。已经 accepted 的结果发布可能仍停在通知调用里，控制者必须等待全部 worker 执行、
任务析构和完整发布/通知返回，再撤target并关闭控制fd。队列closed/empty、
统计、网络截止或 quiescing 都不能替代这个屏障；禁止先关闭/复用通知 fd。

**排空期限不是整个 shutdown 的硬上限。** 当前不取消任务、不强杀或 detach worker；永久
阻塞的任务仍可让 join 无限等待。服务、提交器、结果队列和通知端的生命期不能为赶截止而
缩短。非法时长返回 EINVAL 并保持运行态；已停止后重复调用返回原首因，不重新启动或排空。

## 验证入口

真实注册的 `net.connection_core_drain_*` 用独立黄金线字节、受控同钟和真实 timer 验证排空、慢输出、
等号、重复收紧、原请求期限、时钟/设时/通知故障和资源屏障。
`net.graceful_server_1/2`选择总容量128/256，使用真实indexer快照、单Reactor/worker及查询结果验证执行中和
等待任务、共同绝对截止、拒绝新任务/accept、缓存帧不续读、晚到完整通知及原首因。
`net.graceful_process` 验证真实进程 SIGINT/SIGTERM/定时关闭、参数端点与最终报告。
包装只覆盖静态调用处，不证明任意调度、DSO内部故障、真实OOM、硬实时或生产性能。

显式快照控制启用后，停止还会永久关闭加载准入并排空/join Loader，完成值仍可取；
加载与查询都不被网络期限取消，服务生命覆盖全部加载/查询调用和Core元信息读取，详见[安全发布](SNAPSHOT_PUBLICATION.md)。
