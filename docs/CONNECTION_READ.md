# Connection 读取链路

`siftwing_connection` 将已连接的流 socket、[有界 Buffer](BUFFER.md)、
[增量拆帧](FRAME_DECODER.md)与[Body 准入](FRAME_CODEC.md)串起来。
它 PUBLIC 依赖 `siftwing_socket` 和 `siftwing_frame_codec`；接口权威为
`include/siftwing/net/connection.hpp`。通常由[Listener](LISTENER.md)交付 fd，
所属 I/O 线程构造并串行使用 Connection。

## 所有权和输入

Connection 接管一个非空 `UniqueFd`，禁止复制和移动。构造先检查配置、构造 Buffer，
成功后才移动 fd；配置或初始分配失败时调用方仍拥有原 fd。调用方保证它是已连接的流
socket；本层不额外探测类型或连接状态。`get()` 只借用数字，不能外部关闭或另设拥有者。

Body 上限为 `[2,1048576]`，默认 1 MiB；输入 Buffer 上限固定为 `24+Body上限`。
返回 Frame 拥有原 Body，不借用 Buffer，连接关闭/析构后仍可使用。JSON 对象、UTF-8、
方向及 PING 的 `{}` 在交付时已检查，SEARCH/SUGGEST 等业务字段仍由后续模块验证。
输入上限不等于进程 RSS；Frame 副本、JSON 解析存储、Buffer 增长临时空间仍需内存。

## 读取和门禁

`read_request(byte_budget=65536, attempt_budget=16)` 每次至多交付一个请求：

1. 先检查终止状态和在途门禁，再拒绝任一零预算，零预算不解码/recv。
2. 先尝试解码缓存首帧；完整帧先交付，头非法立即终止。
3. 不完整时，按剩余字节/尝试预算执行 `receive_some`，每次最多 4096 字节，强制
   `MSG_DONTWAIT`。短读继续，EINTR 计尝试并有界继续，EAGAIN/EWOULDBLOCK 立即停止。
4. 最后一次读取后仍解码，请求或头拒绝优先于预算耗尽；半帧保留且不发布。

| ReadStop | 含义及调用方动作 |
|---|---|
| request | request有值，独占Frame；body_error为空才可交业务 |
| request_pending | 上一交付尚未完成；零解码/recv |
| would_block | error为原EAGAIN/EWOULDBLOCK；保留半帧，等待后续可读 |
| budget_exhausted | error为空；保留余量，安排后续读取，不代表队列已经空 |
| invalid_budget | EINVAL、状态保持、零解码/recv |
| peer_eof | 空输入时读到0，关闭fd |
| truncated | EOF残留半帧，关闭fd，不发布部分请求 |
| header_error | 保留DecodeError首因，关闭fd，不猜ID、不扫描magic重同步 |
| io_error | recv或响应send终止错误，关闭fd，保留原errno |
| local_failure | 本地异常曾原样抛出，连接已关闭，后续查询此状态 |
| closed | 显式关闭，后续无I/O |

`attempts` 包含失败、EINTR、EAGAIN和EOF；`bytes` 只计本轮实际从内核取得的字节。
缓存帧交付可二者均为0。字节/尝试预算限制I/O工作，不是JSON解析CPU硬预算。

合法请求和 Body 拒绝都设置 `request_pending()` 门禁。V2.0 每连接一个在途请求；上层在
响应实际写完后调用 `finish_request()`，不可仅在提交任务时解除。
使用[响应输出链路](CONNECTION_WRITE.md)时，输出非空会拒绝解除；排空后仍须显式调用。
原未使用输出API的读取调用方式保持，解除不自动读数据。
如果已经预读下一帧，内核可能没有新数据，调用方必须主动再调用读取，不能只等新 epoll
事件。尚未消费的顺序余量仍有界保存，门禁期间不继续从内核取数据。

Body 错误的 request 仍含受信头/原Body/ID，可调用既有 `make_error_response`；它不是
可执行的业务请求。`json_depth` 属本地资源拒绝，可选择 RESOURCE_LIMIT；其他 Body
错误可选择 INVALID_REQUEST。错误响应本身必须容纳输出预算；上层也可以关闭连接。
读取API不自动构造或发送响应；上层可使用[输出API](CONNECTION_WRITE.md)，不保证它到达对端。

## 终止和异常

头非法、截断、系统读取错误及干净 EOF 自动关闭一次并清空输入和输出。重复读取保存终止原因、
头诊断或原errno，计数为0，不重复关闭；关闭错误不会覆盖读取首因。`close()` 幂等，
返回显式关闭的诊断，不重试 Linux close EINTR。

EOF在缓存完整帧交付和在途门禁解除后继续观察；对端 SHUT_WR 不表示它不能读取响应，
不立即丢弃已交付请求。上层响应完成后继续读取，再处理EOF或末尾截断。

Buffer追加、Frame复制或JSON准入的分配/库异常先终止并关闭，然后原样传播。
已经从内核取走的字节不能回滚，因此调用方不能在该Connection上重试。构造失败则不同：
它还没有取走fd或读取字节，调用方资源保持。已经交付的Frame不受随后终止影响。

## 验证入口和限制

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^net.connection_'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^net.connection_'
```

`net.connection_read` 使用独立手写线字节，验证每个PING分片边界、粘包/缓存调度、四种
请求方向、Body准入、双预算、最大Body/64位ID、半关闭/EOF截断、IPv4/IPv6 Listener接入、
独占资源生命及隔离子进程fd0。`net.connection_failures` 仅在测试目标wrap recv/close，
验证有限EINTR、EAGAIN、其他errno、原错误及单次关闭、门禁零调用；对实际分配路径逐点
抛bad_alloc，核构造源保持、读取异常关闭以及已交付Frame独立。检查不依赖assert，Release有效。

注入证明政策，不证明真实错误时序/资源耗尽。生产目标没有hook。本层没有生产EventLoop、
自动写事件注册、业务执行、worker、ConnectionId/generation、超时或全局限额，
没有任意线程安全或服务性能承诺。有界输出及写意向见[写入链路](CONNECTION_WRITE.md)；
协议闭环和线程归属按独立模块实现。
