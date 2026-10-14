# SearchTask 与 Completion 拥有载荷

`concurrency::SearchTask` 拥有准入后的 SEARCH/SUGGEST 请求，`Completion` 拥有独立响应。公共合同见 [search_task.hpp](../include/siftwing/concurrency/search_task.hpp) 与 [completion.hpp](../include/siftwing/concurrency/completion.hpp)，CMake 目标为 `siftwing_search_task`，只依赖既有帧 codec。

## 准入与独立拥有

`SearchTask::create(Frame&, max_body_bytes)` 先复用 server 帧准入，再限制请求类型为 SEARCH/SUGGEST。合法 PING/SERVER_INFO 返回 `message_type`；不合法帧先返回 codec 的首因。检查严格 UTF-8、JSON 对象和协议字段，保留原始 Body 字节；这不证明 query/key/k 等业务字段合法。业务执行与 JSON schema 属于正式业务接入。

参数预算为 2—1048576 字节，冻结在任务中，并用于成功或错误响应。它只限制单个 Body，不是任务总字节、全局在途量或 RSS。非法预算抛 `invalid_argument`，准入分配异常传播。拒绝或异常保留源全部字段；成功后才移动拥有 Body，源 request_id 归零、Body 清空，type/status 保持。

两个类型禁止复制与默认构造，移动构造/赋值不抛异常；自移动保持，移空对象可以观测、重新赋值或析构。任务移空后预算为零。左值 getter 返回 const Frame 借用，移空返回 nullptr；右值 getter 被删除。借用在移动、赋值或析构后失效，不能跨线程保存。移动本身不提供同步；经有锁队列转交唯一拥有者后才能由另一线程访问，同一对象并发修改需要外部同步。

## 响应与失败

`Completion::success(task, body)` 在调用期间同步借用输入，以 codec 复制并验证响应 Body，映射 SEARCH_RESPONSE/SUGGEST_RESPONSE，原值保留 request_id，status 为零。结果不借用 task 或 Body，因此它们先销毁后仍可使用结果。

`Completion::error(task, status)` 复用六种固定安全诊断生成 ERROR_RESPONSE，不接受任意异常字符串。两工厂都使用 task 的本地预算；错误 Body 超限返回 `body_length`，不递归生成错误。移空 task 优先返回 `request_id`。协议失败返回 `ProtocolError`，分配异常传播且输入保持；不会发布部分结果。工厂不执行查询、不自动把 WorkerPool 的 execute 异常转为响应。

`std::move(completion).take_frame()` 将响应以拥有值取出，源移空；重复取出返回 request_id=0、Body 为空的无效 Frame，调用者必须检查，不依赖其他移空字段值。此接口不编码或发送。

## 接入边界与验证

SearchTask 是数据载荷，不继承 [WorkerTask](WORKER_POOL.md)。真实执行任务可组合它，并明确依赖生命周期；当前没有正式业务调用方。两个类型均无 Connection 指针、fd、回调或索引借用。request_id 只用于关联，允许重复，不能作为连接身份回投。[身份/generation与路由包装](CONNECTION_IDENTITY.md)已有独立组件；独立[CompletionQueue](COMPLETION_QUEUE.md)已定义容量/归属/陈旧结果处理边界；[eventfd结果回投](COMPLETION_WAKEUP.md)已有Sub接入，业务服务接入仍待后续。纯载荷不得直接用于网络回投。

三个真实 CTest 覆盖请求/响应类型、精确字节拥有、关联号边界、移动/移空与右值借用限制、预算两端和错误顺序；多生产者通过测试专属 WorkerTask 和 TaskQueue<Completion> 转交结果，所有源及任务销毁后再逐 id 检查恰一次结果；逐实际 new 分配失败验证输入保持与移动无分配。测试结果队列仅是交接工具，不定义正式结果队列的容量、过期或通知政策。

Debug/Release 全量与本模块 ASan/UBSan、独立 TSan 结果以验证日志为准；分配注入不证明真实 OOM，有限调度不证明所有并发时序。未验证网络发送、连接复用、业务语义、性能或生产资源上限。
