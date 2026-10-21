# 有界 CompletionQueue

`concurrency::CompletionQueue` 是一个固定Reactor目的地的拥有结果邮箱，目标 `siftwing_completion_queue`。公共合同见[completion_queue.hpp](../include/siftwing/concurrency/completion_queue.hpp)，载荷沿用[RoutedCompletion](CONNECTION_IDENTITY.md)。它已通过[eventfd结果回投](COMPLETION_WAKEUP.md)接入ProtocolSubReactor的可选提交器；真实查询业务已由[在线服务](ONLINE_SEARCH.md)接入。

## 发布与容量

生产唯一Reactor使用一个结果队列，默认总容量256，最大65536；旧独立Sub仍限制单实例1024。
合并取消旧每Sub分片，不改变FIFO/拒绝保源/关闭保留/完整发布者寿命门禁，也不承诺原性能。

构造冻结reactor_id并预分配[1,65536]个空槽，0是合法编号。容量计算尚未取出的条目，包括陈旧结果；不计算Body字节、RSS或执行中任务。非法容量抛invalid_argument，预分配异常传播。

try_publish接受独占unique_ptr左值，同锁按closed、invalid_completion、wrong_reactor、full、accepted判断。拒绝与初次加锁异常保持源和队列；accepted才移动拥有者，之后不分配、不抛。消费者可能在返回前已取出，原结果借用不能继续使用。full不覆盖旧条目，不消费新结果，不等空位；调用者须明确处理保留结果。测试中的yield重试是实验选择，不规定生产worker忙循环。

归属只核reactor_id，不查身份表或实际连接。控制者保证不同Sub编号和命名空间生命周期，组件不检测其他队列。所有API可等待mutex，因此没有无锁、硬实时或公平承诺。

## 取出与陈旧结果

try_pop非阻塞按FIFO交出拥有结果，空返回nullptr；FIFO指成功发布的锁内顺序，不保证业务完成顺序。身份和请求/响应字节随拥有者一起转交，内部不暴露借用。

worker不访问Connection或身份表。旧generation可能在发布前或入队后失效；队列不扫描/自动驱逐，陈旧条目仍占容量。所属I/O线程取出后查询当前身份并核Connection和请求状态，陈旧结果在锁外丢弃，释放容量。request_id门禁与正式响应应用已由[Sub回投路径](COMPLETION_WAKEUP.md)实现，[期限门禁](DEADLINES.md)在所属Sub检查；拥有结果和accepted不证明响应已发送。

## 关闭与生命周期

close同锁幂等关闭入口，保留已有条目继续取出，不创建或执行通知，不等待发布者/消费者。status同锁按值观测编号、容量、size和closed。空pop后检查状态可能遇到最后一次发布，消费者结束条件须在同一次status中确认closed且size为0。

队列不可复制/移动。控制者销毁前撤销入口并等待全部调用返回，close或empty不证明调用者静止；析构兜底释放残留结果，不自动执行网络排空。锁内只核状态和移动空槽拥有者，不执行用户回调、不销毁结果、不查连接表。初次mutex失败抛system_error且保持，构造后正常发布/取出/关闭/观测不分配。

## 接入与验证边界

三个真实CTest覆盖容量/环绕FIFO、拒绝保持及错误顺序、跨owner隔离和陈旧结果锁外门禁；真实WorkerPool多生产者向一个结果消费者交接，独立逐id/route/Body核恰一次；发布与close竞争按每次实际回执核所有结果。GNU静态new/mutex包装验证预分配失败、初次锁错误和操作零分配；不覆盖真实OOM、DSO内部new或任意同步设施损坏。

Debug/Release全量及独立ASan/UBSan、TSan结果以验证日志为准。[eventfd通知及Sub应用](COMPLETION_WAKEUP.md)已实现；真实查询执行见[在线服务](ONLINE_SEARCH.md)，[连接期限](DEADLINES.md)已有可选策略，正式服务已支持[限时网络排空的优雅退出](GRACEFUL_SHUTDOWN.md)，任务取消仍留待后续。本队列的独立测试不证明在线搜索已接通。
