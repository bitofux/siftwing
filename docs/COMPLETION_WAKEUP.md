# eventfd结果回投

公共接口见[completion_wakeup.hpp](../include/siftwing/net/completion_wakeup.hpp)和[连接报告](../include/siftwing/net/reactor_report.hpp)，目标`siftwing_completion_wakeup`。它复用[CompletionQueue](COMPLETION_QUEUE.md)、[拥有路由载荷](CONNECTION_IDENTITY.md)和[有限eventfd通知](EVENTFD_WAKEUP.md)。已提供可选请求提交及所属Reactor结果应用机制；真实SEARCH/SUGGEST与生产服务命令见[在线搜索闭环](ONLINE_SEARCH.md)。

## 发布、通知与预算

OnlineServer在唯一循环资源完整就绪后建立`CompletionTarget`，只借用结果队列和同一个Reactor eventfd；该目标不给worker连接、socket或身份表访问能力。

`try_publish_and_notify(queue, source, wakeup_fd)`先按队列合同发布，accepted才在锁外通知。拒绝和初次锁异常保留源、不通知；accepted移空，消费者可能在通知返回前已经取出。通知EAGAIN表示已有可读通知；有限EINTR重试耗尽或其他错误保留诊断，不撤销入队、不重投已消费源。full保留新结果，由调用方处理；函数不等待空位、不规定忙循环或丢弃策略。

唯一Reactor在wait前后有限清通知，再按预算消费结果队列；监听直接交付连接，没有fd移交队列。通知计数不是条目数；结果预算耗尽但队列仍非空时下一轮以零等待继续。清通知有固定尝试预算，仍可读的控制源由LT再次报告；空队列也可能收到迟到通知。即使通知失败，每轮有限epoll等待的后备仍检查队列；100ms不是硬实时保证。

## 可选请求提交与应用

`ProtocolConnectionConfig`提供结果总容量[1,65536]和每轮预算[1,1024]，以及默认空的`RoutedRequestSubmitter*`。旧配置默认继续PING/PONG与受信错误行为。启用者在创建核心前配置submitter，保证对象到完整服务停止返回仍存活；接口只将准入后的SEARCH/SUGGEST与完整ConnectionId作为拥有请求交接，不在I/O线程执行查询或等待任务空位。accepted须移空源，拒绝或提交前异常保留源，每接纳任务至多发布一个结果；异常由Reactor关闭该连接。full/closed返回既有安全OVERLOADED/STOPPING响应，生产等待队列的拒绝、响应排空和恢复准入见[worker饱和策略](WORKER_SATURATION.md)。

接纳后Connection仍有在途请求，所属slot标记等待结果并暂停IN/RDHUP订阅，避免管线请求及半关闭持续触发LT。全关闭ERR/HUP仍可关闭该连接。半关闭的读EOF在响应完成后继续观察。

结果取出后已释放queue锁：Reactor先核身份表全部字段，再核实际slot/Connection及等待状态，最后复用Connection的request_id、成功响应类型和单响应门禁。陈旧身份、当前请求不匹配或重复结果锁外销毁，不解除正确请求门禁。成功复制输出后由所属Reactor按预算发送，输出排空才finish，并主动处理已缓存下一帧。结果入队、应用、发送排空和对端接收是不同事实。

协议允许重复request_id。当前包装没有额外请求代次，同连接再次使用同ID后不能识别违反“每任务至多一个结果”合同的旧副本；不能把连接generation误称请求generation。[期限门禁](DEADLINES.md)已接入：请求到期关连接撤身份，迟到结果拒绝；任务取消仍待后续。

## 停止与所有权

结果队列、连接和epoll/eventfd由OnlineServer::Impl及其所属调用线程管理；worker只借用稳定目标。
stop关闭准入并放弃网络输出，shutdown在同一线程主动排空已在途请求；二者均须完整等待Loader/worker
及最后发布/通知返回，再撤目标和清理Core、残留结果及控制fd。队列closed/empty不能代替完整调用返回。
网络截止不取消accepted任务，也不是总退出硬限，详见[关闭合同](GRACEFUL_SHUTDOWN.md)。

## 验证边界

三个真实CTest使用原创独立24字节编码参考，验证实际Reactor/socket回投、部分收发预算和管线缓存继续、半关闭、同数字fd/同slot/同ID代次复用、错ID/类型及重复结果拒绝；两个独立测试核心实例与真实WorkerPool交接逐客户端拥有Body，静态send/recv/epoll_ctl观测worker不做连接I/O。

定点write/mutex注入验证提交前异常与提交后通知失败、EINTR预算和拒绝不通知；真实饱和eventfd、一个通知对应多结果，以及阻塞在入队后write的晚到发布者验证继续调度和静止屏障。GNU链接包装是当前静态工具链观测，不证明DSO内部、真实OOM、所有调度、性能或正式搜索语义。各配置实际范围及结果以验证日志为准。
