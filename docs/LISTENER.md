# Listener 与有界 accept4 接入

`include/siftwing/net/listener.hpp`是接口合同权威，Listener组件（链接别名`siftwing_listener`）复用
[fd/socket基线](SOCKET_BASELINE.md)。Listener独占监听fd，已交付连接各由调用方UniqueFd独占。
它提供监听创建和接入，不创建EventLoop、Connection、线程、业务处理或codec接线。

## 数值地址与创建

`Listener::create(ListenConfig)`接受数值IPv4/IPv6地址、主机字节序uint16端口、正backlog及
显式reuse_address。默认127.0.0.1、端口0、backlog128、不设置SO_REUSEADDR。
地址仅创建期间借用，不保存string_view；有界栈复制给inet_pton，拒绝空/NUL、DNS、IPv6 zone、
族不匹配和过长文本。IPv6始终设置IPV6_V6ONLY，避免依赖内核默认双栈策略。

先预检，再复用open_tcp_socket原子NONBLOCK/CLOEXEC创建，按需setsockopt→bind→listen→
getsockname；port()报告内核实际端口，0配置允许选择临时端口。backlog请求可能被内核截断，
不是全局连接限额。所有步骤失败包括EINTR均返回generic_category原errno，不隐藏重试，
临时资源由UniqueFd关闭一次；不使用fcntl补标志，不配置SO_REUSEPORT或DNS解析。

Listener不可复制，可noexcept移动；移出/close后的get()=-1、port()=0，fd0合法。
get()只借用，不得外部close或改变NONBLOCK；资源操作属于一个I/O线程或由调用方同步。
Listener的const方法仍会改变内核队列，不能据此推断线程安全。

## 单次和批次接入

`accept_one()`仅一次`accept4(fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC)`，
成功返回UniqueFd，失败包括EINTR返回原errno；空监听者EBADF且无系统调用。
原子设置新连接的两个标志，不依赖监听socket标志自动继承，不获取peer元数据或读取应用字节。

`accept_into(destination,capacity,attempt_budget)`使用已构造UniqueFd数组，容量和预算须为正。
前min(capacity,attempt_budget)个槽位须为空；预检失败EINVAL，全数组保持且无accept4。
空监听者EBADF优先。有效调用填充成功前缀，其他槽位保持，无动态分配或回调。
数组在调用期间须有效、稳定、可写，与监听fd拥有权不重叠。

| 结果stop | 含义 | error | 调用方责任 |
|---|---|---|---|
| drained | accept4返回EAGAIN/EWOULDBLOCK | 原errno | 当前队列排空，等待后续就绪 |
| output_full | 成功前缀填满容量 | 空 | 转交/释放已接入fd，后续继续接入 |
| budget_exhausted | 实际尝试达到预算 | 空 | 让出本轮调度，后续继续 |
| error | 参数或终止系统错误 | 原errno/预检错误 | 保留成功前缀，诊断、退避或停止 |

attempts计每次accept4，包括成功、EINTR、ECONNABORTED及Linux的pending network errors
（ENETDOWN、EPROTO、ENOPROTOOPT、EHOSTDOWN、ENONET、EHOSTUNREACH、EOPNOTSUPP、ENETUNREACH）。
这些错误在批次里消耗预算后继续；accept_one自身仍不重试。其他错误含EMFILE/ENFILE/
ENOBUFS/ENOMEM立即停止，不偷关成功fd、不自动执行reserve-fd技巧或忙重试。
最后一次成功同时满槽和耗尽预算时优先output_full；二者都不证明队列为空。
预检扫描最多min(capacity,budget)个槽，预算是系统调用次数界，不是CPU时间或系统资源硬限。

## 就绪、清理与后续上层

调用方可借用监听fd交给[LT epoll注册](EPOLL_REGISTRATION.md)。预算耗尽后的未接入连接仍在
内核队列，LT会继续报告可读；本层不自动注册或注销监听者。正常上层清理须先DEL再close，
原有dup/已取事件边界仍适用。销毁Listener只关监听资源，已接入连接保持有效。
资源耗尽时必须由后续调度层安排退避/暂停/恢复，避免不断处理同一LT就绪形成忙循环。

## 测试与证据边界

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^net.listener($|_)'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^net.listener($|_)'
```

net.listener用真实IPv4/IPv6 loopback验证端口/标志、V6ONLY/REUSEADDR、重复bind失败、
有界接入与LT剩余队列、移动/close/展开、已交付连接独立生命及隔离子进程监听/接入fd0。
net.listener_failures只在测试链接wrap socket/setsockopt/bind/listen/getsockname/accept4/close，
验证阶段错误释放、预检零调用、原errno、重试风暴有限预算和终止错误后成功前缀保持。
Release使用显式检查；生产库无hook。有限注入证明错误政策，不证明每个errno在每阶段真实
可发生，未证明真实资源耗尽/信号时序、并发/ET/公网/长期服务/性能或完整应用闭环。
