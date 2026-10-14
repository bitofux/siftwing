# ConnectionId、generation 与拥有路由载荷

`net::ConnectionId` 是 reactor_id、slot_index、generation 的纯值组合，无 Connection 指针、fd 或 request_id。generation 为零表示无效；reactor/slot 的零是合法编号。公共合同见 [connection_identity.hpp](../include/siftwing/net/connection_identity.hpp) 和 [routed_payload.hpp](../include/siftwing/concurrency/routed_payload.hpp)。CMake 目标为 `siftwing_connection_identity` 与 `siftwing_routed_payload`。

## 所属线程中的身份表

`ConnectionIdentityTable` 固定预分配 1—1024 个 slot 元数据，不拥有 Connection/socket。所有查询、修改与析构由所属 Reactor 线程串行调用；const 不授予跨线程查询能力，表不能复制或移动。配置非法抛 invalid_argument，预分配异常传播；构造后 activate/retire/matches/lookup 不分配、不抛异常。

activate 对空 slot 建立下一代身份及全表单调 epoll token。拒绝顺序为 invalid_slot、already_live、generation_exhausted、token_exhausted，拒绝保持表。generation 与 token 最大值可使用一次，随后拒绝，不回绕。token 的零保留给 Reactor 控制事件。调用者先激活，再 ADD 并发布 Connection；ADD 失败须 retire，已消费代次/token 不回收。

retire 仅精确匹配当前 reactor、slot、非零 generation 才撤销绑定，重复、陈旧或外表身份不会撤销新连接。关闭后保留代次，复用同 slot 得到下一代。lookup 只从当前非零 epoll token 返回身份值；DEL 不会清空已取出的事件批次，旧 token 必须拒绝。matches 只证明表中当前注册，不证明 fd 仍打开、请求仍 pending、request_id 合法或 deadline 未过。

控制者配置固定且同时可回投的不同 Reactor 编号；旧结果或旧 epoll 批次可能存在时，不能重用 Reactor 命名空间或重建身份表。`ProtocolSubConfig::reactor_id` 默认零保持旧调用行为，组件不自动全球分配编号，也不检测其他实例。启动一次的 Sub 不重启；正式服务控制者接入时仍须保证编号唯一和生产者生命周期。测试多 Sub 控制者按 base+i 配置并拒绝编号溢出。

## 保持协议行为的 Sub 迁移

`ProtocolSubReactor` 内 Sessions 的固定 slot 仍独占 Connection；现在用身份表建立注册元数据，成功 ADD 后保存 ConnectionRegistration，ADD 失败 retire；drop 先撤销身份，再 DEL/close。事件仍携带原有单调 uint64 token，查表得到身份后核当前 slot 绑定才派发，不用复用 fd 数字猜目的地。

PING/PONG、受信错误响应、读写预算、缓存继续、停止与发布者静止屏障保持。可选请求提交与结果应用见[回投说明](COMPLETION_WAKEUP.md)，真实查询业务已由[在线服务](ONLINE_SEARCH.md)接入；没有公开 Connection 借用。独立 ProtocolLoop 保留原有单调 token 的单循环基线；该路径未接跨线程结果，不扩大为身份框架。

## 跨线程包装

`RoutedSearchTask::create(route, SearchTask&)` 先拒绝无效身份，再拒绝移空载荷；拒绝保持源，成功才成对移动身份与拥有请求，不重新解析或分配。数值非零不证明身份可信或存活，调用方从所属表取得身份，接收者还须查表。

`RoutedCompletion::success/error` 同步借用 RoutedSearchTask，复制对应身份并复用既有 Completion 工厂，独立拥有响应。移空输入返回本地 RouteError，codec 拒绝仍为 ProtocolError，分配异常传播且输入保持；不混同线上 ErrorStatus，不任意改写结果目的地。

包装不可复制、移动不抛，自移动保持；移空身份全零且载荷为空。route() 返回身份值；左值 payload() 仅借用到移动、赋值或析构，右值 getter 删除。移动不建立线程间同步，拥有载荷须经同步队列交接。响应在任务销毁后仍有效，但路由可能已经失效。

独立[CompletionQueue](COMPLETION_QUEUE.md)已提供结果归属、容量和关闭排空；[eventfd通知与所属Sub结果应用](COMPLETION_WAKEUP.md)已有接入，校验身份、等待状态及既有Connection关联门禁；真实查询、deadline及优雅退出仍属于后续模块。当前测试 apply 只示范身份门禁与既有 Connection 的组合，不能当作生产回投实现。

## 验证边界

三项真实 CTest 覆盖完整字段/slot 复用/注册回滚/旧 token/旧身份拒绝、预分配故障与激活零分配、拥有包装与局部错误；多生产者验证身份及 Body 经 worker 交接后完整独立。Linux 64 轮复用确知已释放的同一数字 fd、同 slot、同 request_id，旧 Completion 在输出缓冲之前被拒绝且无 peer 字节，新身份结果正常发送。既有多 Sub 测试继续验证实际迁移后的迟到事件、fd 复用、协议及关闭行为。

Debug/Release 全量及 ASan/UBSan、独立 TSan 的范围和结果读取验证日志。未进行 2^64 次耗尽、真实 OOM、所有调度或生产性能实验；溢出拒绝分支经作者源码审读，不冒称运行验证。本节身份专项测试不证明正式业务；结果回投的独立接入与证据边界见[回投说明](COMPLETION_WAKEUP.md)。
