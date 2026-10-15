# Sub Reactor 生命周期

`siftwing_sub_reactor` 提供独立 Sub 线程的启动、停止与回收；公共合同以
[sub_reactor.hpp](../include/siftwing/net/sub_reactor.hpp) 为准，依赖已有 epoll/UniqueFd
及 CMake `Threads::Threads`。当前线程只拥有一个空 LT epoll，不接收客户端 fd，
不监听端口。本文类型保持空生命周期基线；专用连接队列、eventfd与Main协议接入由独立
[ProtocolSubReactor](MULTI_REACTOR.md)提供。worker接入与停止屏障已由[在线服务](ONLINE_SEARCH.md)组合；当前立即丢弃网络输出，优雅排空仍未实现。

## 调用与拥有关系

```cpp
siftwing::net::SubReactor sub;
if (const auto error = sub.start()) {
    // 启动失败已回收线程；调用者处理error。
    return;
}
// start成功表示曾完成线程内资源初始化；运行首因另由status/join报告。
sub.request_stop();
const auto result = sub.join();
// result为空才表示本Sub运行与清理均无错误。
```

对象不可复制或移动，地址须在线程退出前保持稳定。唯一外部控制者串行调用 start、join
与析构；request_stop 与 status 可并发使用，但调用者须保证对象存活，析构前撤销这些
入口并等待已进入调用返回。析构请求停止并 join，不 detach。当前没有用户回调，
正常调用无法从 Sub 线程调用 join；仍防御 self-join 并返回 resource_deadlock_would_occur。

epoll 创建、等待和关闭全部在所属 Sub 线程执行。控制者不能取得 epoll 整数借用；
状态快照只复制状态和错误。状态锁保护报告，atomic stop 传递单调停止请求；
线程句柄的串行使用依赖上述控制者合同，不能据 status 有锁推断 start/join 也可并发。

## 就绪、停止与错误

| 入口/事件 | 合同 |
|---|---|
| 构造 | idle，无线程与 I/O fd |
| start | 只启动一次；等待线程内完整就绪或失败，失败返回前内部 join |
| 再次 start | EALREADY；错误停止或正常停止后均不可重启 |
| idle 时 request_stop | 标志生效，随后 start 返回 operation_canceled，不创建线程 |
| 启动中 request_stop | 若初始化后检查已观察停止则取消并清理；若已发布就绪则允许 start 成功后退出 |
| running | 无注册项；每次 epoll_wait 使用 100ms 有限等待，EINTR 后重新检查 stop |
| request_stop 返回 | 请求已发布；不关闭 fd、不等待线程退出 |
| stopped | 线程内 I/O 已清理，尚不能据此销毁对象；仍须外部 join |
| join 返回 | 线程不再借用对象；首因优先，否则返回关闭诊断；重复 join 保持结果 |
| idle 时 join | 无操作，仍可 start；运行中 join 不隐含 stop |

100ms 是单次等待参数，包含调度延迟的退出时间没有硬实时保证。每次 EINTR 都回到
停止检查，不因信号不断到达而跳过检查。本类型保留有限等待基线；带eventfd的连接服务由独立ProtocolSubReactor提供。

线程创建 system_error 保留 code，bad_alloc 映射 not_enough_memory，其他异常映射
io_error；epoll_create1 失败原 errno 不重试，epoll_wait 非 EINTR 错误终止运行。
线程边界捕获操作异常，显式 close 在所属线程执行一次。first_error 与 cleanup_error
分开保存，关闭错误不覆盖首因；Linux close 即使 EINTR 也不重试。
控制者的 mutex、条件变量或 join 系统错误仍可抛出；同步设施自身失效没有恢复保证，
noexcept 析构/线程收口无法完成时会 terminate，不以 detach 隐藏失败。

控制者须逐个汇总固定拓扑的 Sub 启动结果，全部就绪后才开放接入；部分启动失败则
请求停止已启动者并逐个 join。这里提供单个 Sub 原语，实际 Main 接入门禁和服务运行中
错误协调由[多Sub协议组合控制者](MULTI_REACTOR.md)展示；一个 Sub 的 start 成功不证明整个服务 ready。

## 验证与边界

真实 CTest `net.sub_reactor` 覆盖反复启动/停止、并发停止与状态读取、两个独立 Sub、
析构回收及 `/proc/self/fd` 无增长。`net.sub_reactor_failures` 用测试专属链接器 wrap
覆盖创建 errno、线程状态分配失败、启动中取消、线程边界异常、等待首因、关闭错误、EINTR 与部分启动
回滚，并证明 epoll 在同一非控制线程创建和关闭，内核 fd 已关闭且关闭仅一次。
检查不依赖 assert，Release 下仍执行。

注入证明错误政策，不代表真实资源耗尽或任意并发时序；这里没有网络收发或队列，
不外推业务吞吐、性能、连接关闭、通知借用安全或服务优雅退出。旧
[ProtocolLoop](PROTOCOL_LOOP.md)仍是独立单线程协议基线。
