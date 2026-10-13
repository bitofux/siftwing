# Connection 写入链路

`siftwing_connection` 在[读取与请求准入](CONNECTION_READ.md)基础上，用既有
[Buffer](BUFFER.md)保存完整响应的未发送后缀。接口合同见
`include/siftwing/net/connection.hpp`；依赖仍通过 CMake 目标声明，无新增第三方。

## 所有权、容量与关联

Connection 独占 socket、输入和输出 Buffer，由所属 I/O 线程串行操作，不能复制或移动。
每个 Buffer 的上限独立为 `24+max_body_bytes`，默认 Body 上限 1 MiB；输出延迟分配。
这不是进程 RSS 上限：调用者 Frame、临时编码字符串、JSON 验证及扩容会增加峰值内存。
构造所有 Buffer 成功后才接管 fd，失败时调用方资源保持。`get()` 是借用；使用本输出
API 时不能另行 send 打乱顺序或外部 close，fd 0 合法。

每次读取交付保存独立的请求 ID、类型和 Body 拒绝标记，不依赖调用者之后可修改的 Frame。
`queue_response(const Frame&)` 先检查 closed、no_request、response_pending，再检查
request_mismatch，最后由客户端接收方向的 codec 检查协议准入。ID 必须匹配；成功类型
显式对应 PING/PONG、SERVER_INFO、SEARCH、SUGGEST。ERROR_RESPONSE 可关联所有请求，
其 status、Body、UTF-8/JSON 等仍须合法；Body 被拒绝的请求只允许错误响应。

成功返回 queued，完整线字节已复制，源 Frame 随后可修改或销毁。拒绝无 send 且状态保持；
编码/JSON/Buffer 分配异常原样传播，状态保持，可重试。输出为空才可接纳第一响应，
而且每请求至多一响应：排空后、finish 前再次入队仍返回 response_pending。

## 有界发送与写事件

`flush_output(byte_budget=65536, attempt_budget=16)` 无分配且 noexcept。
先检查终止、空输出，再拒绝非空输出的任一零预算（EINVAL，无 send，状态保持）。
调用 `send_some` 强制 `MSG_DONTWAIT|MSG_NOSIGNAL`，即使传入阻塞 fd 也不等待缓冲区。
每次请求长度受剩余字节预算限制，只有内核确认的前缀被 consume；每次 send 都计尝试。

| WriteStop | 状态与调用方动作 |
|---|---|
| drained | 输出已空；去掉写意向，显式 finish，再主动处理缓存下一请求 |
| would_block | 原 EAGAIN/EWOULDBLOCK；保留精确后缀及写意向，等待可写 |
| budget_exhausted | 尚有后缀；保留写意向，安排下一轮，不表示内核已满 |
| invalid_budget | EINVAL；零 send，状态保持 |
| io_error | 原 send errno，非空 send 返回 0 用 EIO；单次关闭并丢弃输入/输出/门禁 |
| closed | 因其他原因终止，零 send；可能携带原终止 I/O 诊断 |

短写后继续；EINTR 计预算并有界重试，EAGAIN 立即停止。最后一次 send 排空时，
drained 优先于两个预算到达。发送失败后重复 flush 返回原 io_error、零计数，无重复
send/close；read_request 也可查询同一终止 I/O 首因，关闭错误不覆盖它。
显式 close 或读取终止同样清空输出及写意向。

`needs_write()` 表示 fd 有效且输出非空，只是 EPOLLOUT 意向。所属上层必须用
epoll MOD 实际更新掩码，失败时关闭；本类不注册 epoll。
单线程调用者见[协议Loop](PROTOCOL_LOOP.md)，实际订阅由它管理。
入队后可立即 flush，EAGAIN 或预算后保留 EPOLLOUT，排空后移除，避免 LT 可写空转。
输出排空仅表示已交给内核，不证明对端接收、解析或业务完成。

输出非空时 `finish_request()` 拒绝；排空不会自动解除门禁。上层显式 finish 后，
必须主动调度已缓存的下一完整帧，不能只等待新的 EPOLLIN。未使用新输出 API 时，
既有读取调用方式保持。对端 SHUT_WR 后仍可收响应，finish 后才继续观察读取 EOF。

## 验证入口与限制

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^net.connection'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^net.connection'
```

`net.connection_write` 用独立手写线字节验证四响应类型、错误关联、单响应门禁、逐字节
预算、最大帧、小 SO_SNDBUF 的实际短写/EAGAIN、真实 epoll 掩码切换、IPv4/IPv6 半关闭
响应及隔离 fd 0/默认 SIGPIPE。`net.connection_write_failures` 只在测试目标 wrap
send/close，以真实前缀发送配合有限 errno/零进展注入验证偏移、双预算、首因和单次关闭；
逐实际分配点 bad_alloc 验证入队强保证，检查 flush 无分配。Release 检查不依赖 assert。

注入验证错误政策，不能证明真实 OOM 或任意错误时序。Connection自身不提供 Loop、worker、业务
字段、跨线程完成、连接 generation、高低水位、全局限额或超时，不承诺任意线程安全、
服务容量或性能；这些需相应后续模块明确启动。
