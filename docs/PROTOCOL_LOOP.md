# 单循环协议 I/O 基线

`siftwing_protocol_loop` 将 [Listener](LISTENER.md)、[LT epoll](EPOLL_REGISTRATION.md)
与 [Connection 读取](CONNECTION_READ.md)、[响应输出](CONNECTION_WRITE.md)接通。
接口合同见 `include/siftwing/net/protocol_loop.hpp`。一个调用线程独占整个 Loop 的资源，
本类不创建线程，不允许并发调用、复制或移动。

## 请求与响应

这是用于验证基础协议 I/O 的固定探针：合法 PING 产生 PONG/`{}`，相同 request_id。
合法 SERVER_INFO/SEARCH/SUGGEST 在本探针返回 INVALID_REQUEST；它尚未接业务服务，
不把空对象当作成功搜索结果。受信头下的 Body 错误也返回 INVALID_REQUEST，
JSON 深度拒绝使用 RESOURCE_LIMIT，固定诊断由既有 codec 构造。

非法头、超长声明或半帧 EOF 关闭该连接，不猜 ID；错误响应无法容纳 Body 预算时
也关闭，不递归生成错误。局部分配异常映射本地 ENOMEM 并关闭连接，不向对端暴露
任意异常文本。一次输出排空只说明内核接受，不能证明对端业务处理完成。
这不改变[协议线格式](PROTOCOL.md)或后续业务字段合同。

## 资源、身份与有限调度

`ProtocolLoop::create(listen, config)` 返回完整 `unique_ptr<ProtocolLoop>` 或原系统错误。
启动配置非法返回 EINVAL 且不调用系统；slot/对象分配异常传播，已创建资源由 RAII 释放。
固定 slot 数为 `[1,1024]`，默认 128；每连接 Body 上限 `[2,1048576]`，默认 1 MiB。
每次接入至多 16 个 fd，尝试预算默认 16、范围 `[1,1024]`。读写字节预算各默认 65536，
范围 `[1,24+1048576]`，read/flush 各自尝试预算默认 16、范围 `[1,1024]`。
这是当前基线的局部存储与调度限额，不是进程 RSS、解析 CPU 或服务容量承诺。

Loop 独占监听、epoll 与 slot 中的 Connection，不向调用方暴露客户端 fd。有效资源先
DEL 再 close；Connection 自行关闭后不再用旧 fd 数字 DEL。监听 token 为 0；连接使用
单调递增、不复用的 64 位 token，耗尽即终止，不回绕。已取出的事件按存活 token 派发，
过期事件被忽略。这是本地注册身份，还没有异步 ConnectionId/generation 框架。

`run_once(timeout_ms)` 每轮：

1. 先处理上轮缓存帧或读预算留下的 runnable slot，每连接至多一次。
2. 执行一次 epoll_wait，最多取得 64 个 LT 事件。存在缓存工作时实际 timeout 为 0，
   否则使用调用方值；`-1` 表示无限等待，小于 `-1` 返回 EINVAL 且状态保持。
3. 接入监听或服务存活连接。已经由缓存服务过的连接不会再被该轮事件双重服务；每连接
   每轮最多交付一个请求，read/flush 分别受双预算限制。
4. 响应入队后立即尝试发送；短写保留后缀。输出非空时关注 **EPOLLOUT**，暂停 IN/RDHUP。
   FIN 的 RDHUP 是持续就绪，慢写期间保留它会造成 LT 空转。排空后显式 finish 并恢复
   IN/RDHUP，已预读的下一帧置 runnable，下一轮主动继续。
5. 连接容量满时 MOD 暂停监听 IN；连接终止腾位后恢复 IN，继续处理内核待接入队列。
   掩码未变化时不重复 MOD。

| 返回 LoopStop | 调用方动作 |
|---|---|
| progress | 一轮工作结束，可继续；可能只有陈旧事件，没有响应完成承诺 |
| idle | 本次 wait 无事件且无缓存工作 |
| interrupted | 原 EINTR，无隐藏重试，Loop 保持，可由调用者安排下一轮 |
| invalid_timeout | EINVAL，无 wait，状态保持 |
| failed | 等待、接入资源或监听注册失败，全部资源已关闭；重复查询保首因、零 I/O |
| closed | 显式关闭后查询，零 I/O |

客户端 ADD/MOD 失败及局部读取/响应异常关闭该连接，保留本轮首个局部错误，其他连接
继续。接入分配失败、accept 资源错误、监听暂停/恢复 MOD 失败或 wait 错误终止整个
Loop，避免持续资源耗尽下忙重试。关闭诊断不覆盖原失败。显式 `close()` 立即丢弃全部
连接和输出，返回首个清理错误，无 Linux close EINTR 重试；它不是优雅排空。

LoopStep 提供每轮接入、请求、排空及 I/O 统计。读取异常不返回 ReadResult，因此那条
异常路径未返回的 recv 计数无法计入；这些统计不用于完整流量审计。

## 运行探针与验证

```sh
./build/debug/siftwing-protocol-probe --help
./build/debug/siftwing-protocol-probe 0 1000 10
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^net.protocol'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^net.protocol'
```

探针只监听 `127.0.0.1`，依次可指定端口、有限轮次和等待毫秒，默认 `0 1000 10`。
stdout 的 `PORT <实际端口>` 可供独立客户端连接；结束输出 accepted/completed 计数。
轮次结束立即关闭，无后台线程或信号处理。参数非法退出 2，系统/资源/输出失败退出 3，
正常及成功 help 退出 0。`completed` 是响应交给内核的数量，含错误响应。

`net.protocol_loop` 用独立手写线字节验证 IPv4/IPv6、TCP 分片/粘包、缓存主动调度、
逐字节输出预算、两连接公平推进、半关闭、坏头/截断/RST、最小预算与监听容量暂停/恢复。
`net.protocol_loop_failures` 仅测试目标 wrap 系统调用，验证零调用预检、EINTR预算、
注册和资源失败、短写/EAGAIN 掩码、缓存覆盖无限 wait、过期/重复 token、清理首因及
实际分配点异常；注入不证明真实 OOM 或任意时序。
`net.protocol_probe_process` 由独立 Python TCP 客户端验证真实进程的线字节、半关闭、
有限退出、参数和 help 输出错误。Release 检查不用 assert。

本Loop不组合Main-Sub、worker、eventfd、在线搜索或跨线程完成；相应实现见
[多Sub集成](MULTI_REACTOR.md)和[在线服务](ONLINE_SEARCH.md)。底层Connection已有
[输出水位](OUTPUT_BACKPRESSURE.md)，本Loop沿默认配置；本页测试不单独证明水位合同。
deadline、优雅退出、正式网络客户端和性能结论仍未提供。

Main/Sub协作遵循[线程拓扑与所有权合同](THREAD_TOPOLOGY.md)。本 Loop 保持单线程
基线，不直接共享到多个线程；Sub 的连接 I/O 与 Main 的监听接入分别组合既有组件。
