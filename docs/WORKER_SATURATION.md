# worker等待队列饱和

生产`OnlineServer`的`task_capacity`（命令行`--task-capacity`）是等待任务条目数，范围1..1024，默认128；不计执行中的任务、Body字节或RSS。固定worker数与等待容量分别限制不同资源。

所属Sub通过`WorkerPool::try_submit`尝试交接SEARCH/SUGGEST。队列满就返回`full`并保持源，Sub用原`request_id`返回固定`ERROR_RESPONSE`、状态`OVERLOADED`（2）、Body `{"message":"overloaded"}`。提交可以短暂等待队列mutex，但不等空位，不承诺无锁、硬实时或公平性；不创建重试任务、不扩容、不占住Sub等worker。

错误响应继续服从[单在途门禁](SINGLE_INFLIGHT.md)和[输出硬限](OUTPUT_BACKPRESSURE.md)：完整排空后才finish并继续缓存请求，PING也沿同连接顺序。错误超硬限时尝试同ID固定RESOURCE_LIMIT一次，仍不适配或分配失败只关闭该连接。因此明确过载响应以能够构造、排队和发送为条件。其他连接独立推进；释放队列容量后，新请求重新尝试准入，没有永久拒绝状态或容量预留。客户端重试须自行决定，本服务不提供自动重试或幂等业务承诺。

队列关闭使用STOPPING；业务工作预算使用RESOURCE_LIMIT，均与等待队列full有区别。completion结果队列满仍沿现有首因安全停止策略，见[在线闭环](ONLINE_SEARCH.md)；本合同没有把它改成OVERLOADED，也不提供全局字节准入、deadline、取消或优雅排空。

两个真实CTest `net.worker_saturation_1`和`net.worker_saturation_2`分别运行1/2个Sub、1个worker、1个等待槽。测试经正式indexer生成原创公开输入的快照，启动真实OnlineServer/TCP；GNU静态链接包装暂停一次真实SearchService执行，观察真实try_submit。首任务执行中、次任务排队后，SEARCH与SUGGEST确定命中full，独立黄金线字节核同ID错误、缓存PING和SHUT_WR/EOF；两条健康连接各8个PING在worker放行前完成，并确认各Sub处理了流量。每轮7字节输出覆盖错误跨轮排空。释放后执行真实服务，确认原任务完成、新请求恢复准入，join后精确核三次执行、两次拒绝、无重投和fd回基线。

这套同步测试证明上述受控条件，不是吞吐基准、延迟上界、任意调度或公网拥塞证明。测试依赖当前Linux/GNU/Clang静态符号ABI；没有为测试增加生产接口。
