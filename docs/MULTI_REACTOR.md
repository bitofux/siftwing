# 多 Sub 协议集成

[ProtocolSubReactor](../include/siftwing/net/protocol_sub_reactor.hpp)将专用 fd 队列、eventfd、
LT epoll 和 Connection 组合在一个固定 Sub 线程中。所有 Sub 完整就绪后，
[MainReactor](MAIN_REACTOR.md)在另一个所属线程创建监听并轮询移交。
真实链路为 TCP → Main accept → 目标 queue → eventfd → Sub Connection → codec → 响应 send。
Main 不读取业务，Connection 从构造到释放始终归属同一个 Sub。

原 [SubReactor](SUB_REACTOR.md)空生命周期和 [ProtocolLoop](PROTOCOL_LOOP.md)单线程基线保持。
新类型提供独立接口；测试专用[组合控制者](../tests/support/multi_reactor_fixture.hpp)展示启动、
运行错误汇总和停止顺序。尚未推出正式服务命令、worker、搜索业务或响应优雅排空。
当前只响应 PING/PONG 和受信 ERROR，其他合法业务请求返回 INVALID_REQUEST。

## 固定容量与职责

每个 Sub 对象预分配自己的 queue，Sub 线程内建立定长 Connection slot、epoll 和普通非阻塞 eventfd。
handoff_capacity、max_connections、handoff_budget、io_attempt_budget 都为 1—1024；
max_body_bytes 为 2—frame_max_body_bytes，读写字节预算为 1—24+frame_max_body_bytes。
配置在预分配前完整验证，非法值抛 invalid_argument，无线程或 I/O 创建；分配异常传播。

队列满时仍由 Main 关闭未提交源，不改投；Sub 的连接 slot 满时，在 Sub 关闭已提交但未注册的源。
两者是不同的拒绝边界，不迁移活跃连接。通知计数不决定 pop 次数，每轮先清通知再从真实 queue
取最多 handoff_budget 个条目。queue 非空时 wait 使用 0，空时最多 100ms。

Connection 注册使用单调不复用 token，0 专供通知端，不按 fd 数字派发陈旧事件。
每连接每轮最多服务一次、交付一个请求；输入缓存和读预算余量主动继续。
输出非空只订阅 OUT，排空并 finish 后恢复 IN/RDHUP，避免半关闭慢写空转。
读写复用 Connection 的字节/尝试双预算，不把单次 send 成功当作对端接收证明。

## 启动与发布者屏障

| 接口 / 状态 | 调用者与合同 |
|---|---|
| 构造、start、target、release_publishers、join、析构 | 唯一外部控制者串行调用，对象不可复制或移动 |
| start | 等完整初始化报告，启动失败内部 join；成功只证明曾就绪，随后仍可能失败 |
| target | running 时返回 queue 与 wakeup 的稳定借用，否则 ECANCELED；不保证之后仍健康 |
| request_stop / status | 可并发，但调用者须保证 this 存活；内部通知借用与撤销同 mutex |
| quiescing | 接入 / 业务已停止，首因已发布，资源保留等待外部发布者静止；不是可销毁 |
| release_publishers | 控制者承诺所有外部发布与通知已返回、旧 target 借用已撤销；同时置 stop，幂等 |
| stopped / joined | 前者已在线程内清理，后者线程也已回收；ready 后 join 须先 release |

raw target 借用没有自动引用计数。控制者不能凭 queue closed/empty、Main 停止 accept 或
Sub 已报告错误，就让 Sub 关闭通知端。停止的真实顺序是：

1. 请求 Main 停止接入，等待其所属线程全部发布、通知、清理和析构返回。
2. 完成最后目标控制通知，撤销其他 raw target 借用并等待已进入调用返回。
3. 对每个 Sub 调用 release_publishers，明确允许最终清理。
4. Sub 在线程内 DEL / close Connection，drain / close queue 残留，撤销内部通知镜像，再锁外关闭 eventfd 和 epoll。
5. 外部逐个 join，撤销并等待并发 status / request_stop 调用，再销毁对象。

release 不是自动检查借用者的动作；过早调用违反合同，遗漏调用会让 join 等待。
析构的 stop/release/join 仅在已无外部发布者的前提下兜底。内部 stop 的通知 mutex 保护其 fd
撤销，不能代替 raw target 屏障，也不延长对象生命期。通知写失败仍保留 stop 标志，100ms
单次等待为后备；没有包含调度延迟的硬实时保证。

## 错误与清理

部分 Sub 启动失败，不创建 Main 监听；已启动者按屏障停止并 join。
Main 创建失败也回滚全部已就绪 Sub。Sub 创建 / 连接构造或 ADD / 全局 wait / 通知端错误
终止该目标，先发布首因，再关 queue，防止 Main 的 ECANCELED 遮盖原 Sub 错误。
固定目标的运行失败由控制者停止整个拓扑，不静默缩容。

客户端坏头、EOF、读写失败、响应分配或 MOD 错误属于局部连接关闭，记录 first_connection_error。
完整合法头的 Body 拒绝保留 ID 并返回 ERROR；不是全 Sub 失败。
启动 / 运行首因和 cleanup_error 分开，Linux close 一次，不重试 EINTR。
停止为立即丢弃，不承诺已提交请求或响应完成。

最终 queue drain 或同步设施不可恢复失效时，不能保证目标线程完成清理；noexcept 线程收口 / 析构
可能 terminate，不能悄悄把残留 fd 留到控制者析构。此边界与正常可恢复 I/O 错误分开。

## 验证入口与限制

CTest 注册 net.multi_reactor、net.multi_reactor_concurrency、net.multi_reactor_failures，Release 仍执行检查。
独立 24 字节黄金编码检查真实 IPv4/IPv6、分片、粘连、半关闭、坏 Body / 头和固定目标轮询。
测试包装 accept / 注册 / recv / send / close，独立核 Main 无业务 I/O、Sub 不跨目标操作、注册 token
不复用、每个源只关闭一次；另以真实 fd 复用和陈旧 token 注入验证派发边界。

并发客户端 / 状态观测 / 停止、部分启动和 Main 创建回滚、运行 / ADD / MOD / 分配 / 通知错误、
晚到发布通知屏障、32 个残留队列条目和 stop 通知失败后备均有专门场景，前后核 /proc/self/fd。
线程启动路径的分配注入覆盖状态构造失败；不声称拦截 libstdc++ 内部 pthread_create 或真实线程资源耗尽。
链接包装和 Linux 观测是测试机制，不是 ISO C++ API。未证明任意调度、公平性、吞吐、
真实 OOM、完整搜索服务或 worker 的生命周期。
