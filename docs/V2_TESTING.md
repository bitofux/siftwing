# V2 故障与并发测试

> 历史资料：以下描述迁移前Main/Sub架构及当时组件/测试，相关公开API和纯旧拓扑测试已退役，
> 不保留兼容层。当前实现/测试/调试请读[单Reactor拓扑](THREAD_TOPOLOGY.md)、
> [在线服务](ONLINE_SEARCH.md)及[测试指南](TESTING.md)。原历史实现由迁移前commit定位，旧成绩不代表新架构。

测试组合真实 Main/Sub、worker 和 SearchService；输入使用公开原创夹具建立的真实快照，
响应以独立24字节大端参考和 JSON 断言验证。实际通过范围以对应运行日志为准，测试存在
本身不证明已通过。所有行为断言在 Release 中仍执行。

| 合同 | CTest 入口 | 关键证据 |
|---|---|---|
| 真实服务的 fd/slot/id 复用 | `net.v2_fault_concurrency_1/2` | worker 门保持旧任务；RST 后同 fd、同 Sub/slot/id、新 generation 的请求仍 pending 时收到旧结果；旧结果必须 stale，新请求结果与缓存 PING 有序 |
| 故障与并发业务隔离 | 同上 | 容量1的真实等待队列 full 保持源、拒绝任务不执行；慢输出与半帧头/Body保持时，四客户端完成四种命令及缓存帧，再恢复各自进度 |
| 内核慢读与输出预算 | `net.output_backpressure_limits/sub/tcp` | 高低水位/硬限、部分输出、真实 TCP 小缓冲慢读时其他连接推进 |
| 单在途与半关闭 | `net.single_inflight_*` | pending 不读取或交付下一请求，SHUT_WR 后仍完成完整在途请求 |
| worker 饱和与恢复 | `net.worker_saturation_1/2` | 真实队列 full、OVERLOADED、健康 PING 与容量恢复 |
| 身份及结果门禁 | `net.connection_identity*`、`net.completion_wakeup*` | 旧 generation/token 拒绝、通知、结果分类与完整发布者屏障 |
| 空闲/请求期限 | `net.sub_deadlines`、`net.online_deadlines_1/2` | 原总期限不因进展延长，任务不取消，晚结果拒绝 |
| 正常关闭与故障回滚 | `net.graceful_*`、`net.online_search_*`、`net.multi_reactor*` | 共同排空截止、晚通知、线程/fd清理、启动/运行首因及设施故障 |
| 客户端协议边界 | `net.client_*` | 真实四命令、部分 I/O、坏响应、固定总期限与输出错误 |

`net.v2_fault_concurrency_1/2` 各运行两个独立服务生命周期。复用场景通过真实 `accept4`
后在已确认空闲的保留高 fd 上 `dup3`，不会覆盖其他对象；路由和代次仍由生产身份表生成。
旧与新查询实际执行，包装只观测纯值身份、控制首个 worker 的返回时机及统计真实提交结果。
结果必须按 `received = applied + stale + rejected` 分类；关闭后核对任务数、
`admitted = dropped`、`active = 0`、joined 以及整个生命周期的 `/proc/self/fd` 基线。

混合场景先真实发送 PONG 的5字节前缀，再为该服务端 fd 返回受控 EAGAIN，证明剩余输出
存在时不读取下一请求，健康连接仍可完成业务。它不把小响应恰好进入内核缓冲当作慢读证明；
真实内核慢读由独立 `output_backpressure_tcp` 覆盖。两次 send 和有限读写预算用来制造增量
处理机会，不保证 TCP 分段或 recv 边界。所有等待都有失败看门狗，不能作为延迟基准。

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^net\.v2_fault_concurrency_'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^net\.v2_fault_concurrency_'
```

ASan/UBSan 与 TSan 使用独立 Debug 构建。TSan 只覆盖受测执行中工具可见的数据竞争，
仍需身份、顺序、计数和资源断言；无报告不能证明所有调度、无死锁或业务正确。静态 GNU
`--wrap` 不覆盖 DSO 内部调用，也不证明真实 OOM、所有内核错误或生产性能。全局
new/delete 替换的故障测试须按具体工具相容性选择范围，不能把相关集成通过称为全项目插桩。
优雅退出的网络期限仍不限制永久阻塞 worker 的 join 时长。
