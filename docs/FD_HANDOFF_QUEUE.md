# fd 有界移交队列

`FdHandoffQueue` 是独立的固定容量组件，公共合同见
[头文件](../include/siftwing/net/fd_handoff_queue.hpp)。它实现
[线程拓扑合同](THREAD_TOPOLOGY.md)中的 fd 所有权提交；当前尚未接入
[Sub Reactor](SUB_REACTOR.md)或Main；已有独立[eventfd通知组合函数](EVENTFD_WAKEUP.md)。

构造要求正容量，预分配全部 `UniqueFd` 环形槽位。零容量抛 `invalid_argument`，
超出容器可表示范围抛 `length_error`，分配失败抛 `bad_alloc`。构造后的正常返回路径不分配；
标准库构造加锁异常时可能分配，不在本层无分配承诺内。
对象不可复制或移动。所有操作使用同一个 mutex，锁内只检查状态并移动条目；
不等容量或条目，但可以等待锁，不能作为无锁或硬实时接口使用。

| 操作 | 结果与所有权 |
|---|---|
| `try_push(source)` | 依次检查关闭、空源、满；分别返回 `closed`、`invalid_fd`、`full` 并保持源。成功返回 `accepted`，源为空，队列接管 |
| `try_pop()` | 空时 `nullopt`；否则按 FIFO 提交顺序返回拥有型 `optional<UniqueFd>`，调用方在锁外处理 |
| `close()` | 幂等且不可重开，禁止后续提交，保留已有条目供消费 |
| `status()` | 按值复制容量、大小、关闭标志，不能据此保留空位或条目 |

加锁失败可抛 `system_error`，发生在状态与所有权修改之前。成功移动是唯一提交点，
所有检查和关闭入口都受同一锁保护：先提交则消费者负责，先关闭则生产者保留。
mutex 同时建立提交前写入与取出后读取的可见性；每个源拥有者仍须由其生产者独占。
并发发布者之间的 FIFO 由实际提交先后定义，不由开始调用的墙钟顺序定义。

```cpp
siftwing::net::FdHandoffQueue queue(64);
// accepted 后 source 为空；其他结果由生产者显式处理/关闭 source。
const auto result = queue.try_push(source);
// 消费者取出拥有值；处理、注册或关闭发生在队列锁外。
if (auto fd = queue.try_pop()) {
    const auto error = fd->close();
    // 在所属线程记录 error；close 一次，即使 EINTR 也不重试。
}
```

组件支持多个发布者，由调用方安排一个目标消费者；它不创建线程、不绑定消费者身份，
也不唤醒消费者。后续通知是独立阶段，入队成功后通知失败不能收回或重新关闭源 fd。
队列自身测试使用主动检查；独立eventfd组合的真实epoll消费者验证见[eventfd说明](EVENTFD_WAKEUP.md)，服务接入仍由后续模块完成。

正常退出协议是：关闭发布入口，撤销发布者并确认已进入调用返回，目标线程取完并关闭
遗留条目，join 后销毁队列。所有调用须保证对象存活；析构不能与调用并发。
析构的 RAII 兜底会在析构线程关闭未取出的 fd 并忽略诊断，它不能替代目标线程清理。

CTest 注册 `net.fd_handoff_queue`、`net.fd_handoff_queue_concurrency` 和
`net.fd_handoff_queue_failures`。测试使用真实 fd/socket，覆盖 FIFO/绕环/容量、fd 0、
多生产者可见性与各自顺序、关闭提交竞争、目标线程 drain、恰好关闭一次、析构兜底，
以及测试专属的分配/加锁/关闭诊断注入。运行这些测试不能证明 Main/Sub 连接集成、
通知可靠性、吞吐、公平性或任意调度下的硬实时期限。
