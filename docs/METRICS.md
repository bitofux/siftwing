# 最小服务指标

生产服务持续采集连接、网络、请求、队列、worker、查询延迟、缓存、索引及关闭指标。
C++控制线程通过`OnlineServer::metrics()`取得拥有值；该值不借用连接、任务或快照对象。
指标不依赖日志级别，`SERVER_INFO`仍返回原有固定元信息，不提供动态指标。

## 采样与导出

[结构化日志](LOGGING.md)在info/debug启用且健康时，控制循环每至少1秒导出一次，
成功完成关闭屏障后再导出最终样本。没有新增线程；忙循环或同步日志I/O可延迟采样，
不补发错过的周期，不保证实时性、持久化或对端收到。off/warn/error不导出。
观察加锁、序列化或输出失败不改变业务响应；序列化/I/O失败会沿原合同停用日志。

普通日志保留原17键；`event="metrics_snapshot"`使用同一信封，额外包含：

| 字段 | 含义 |
|---|---|
| metrics_schema_version | 数字1 |
| sample_id | 本进程递增uint64十进制字符串，饱和后不再增加 |
| metric_family | 下表固定族名 |
| values | 下表固定字段对象 |

每族一行，每行含LF仍不超过2048字节。所有计数、深度、微秒和桶元素均为十进制字符串，
布尔值保持JSON布尔。index_version在信封中保留完整版本；不使用查询、路径、连接或请求作为标签。
十行写入不原子，其他日志可穿插，失败或轮转淘汰可留下不完整样本；消费者须按sample_id检查族完整性。

## 统计口径

| 族 | values字段与口径 |
|---|---|
| connections | accepted为Main实际接入；handed_off为成功移交；rejected合并Main拒绝及Sub注册前拒绝；admitted为Sub实际注册；closed为注册后关闭；active为当前注册数；discarded为关闭时未注册的移交残留 |
| network | received_bytes/sent_bytes为正recv/send的实际字节，含头、坏输入及部分发送；input_bytes/output_bytes为已发布Sub报告中的活跃连接用户缓冲可读字节总量；input_high_bytes/output_high_bytes为所有连接中最大的单连接历史可读字节；protocol_errors为坏头、EOF截帧、完整帧Body准入错误次数，不含业务schema校验错误 |
| requests | by_type依次PING、SERVER_INFO、SEARCH、SUGGEST，统计完整受信交付，含坏Body；responses按status 0..6统计实际响应入输出队列次数；drained为输出排空且finish成功次数，不证明对端收到 |
| queues | tasks为等待worker的任务数，不含execute中的任务；task_capacity为固定槽位容量；handoff/completions为各Sub移交/结果队列即时深度之和 |
| workers | count为固定线程数；busy只覆盖execute调用；succeeded/failed沿WorkerPool正常返回/抛异常计数，业务错误返回仍属于succeeded |
| search / suggest | count为实际服务execute次数，failures为非成功Body结果（含异常映射）；sum_us/max_us为steady_clock微秒耗时和/最大值；buckets为六个互斥桶；collection_failed表示至少一次查询统计因同步异常丢失 |
| cache | hits/misses/evictions/entries/bytes来自同锁LRU统计；bytes为charge，非RSS；缓存关闭时沿原缓存合同，不能以业务请求数推测miss |
| index | documents/terms为只读搜索索引文档/词项数，recommendation_terms为独立推荐词项数，版本读取信封index_version |
| shutdown | started/finished表示进入关闭与完成worker/Sub屏障；elapsed_us为关闭完整耗时，含join；drain_timeouts/drain_responses为网络排空截止/完成数；stale_completions/discarded_completions为陈旧/停止后销毁的结果数 |

status槽位依次为成功0、INVALID_REQUEST=1、OVERLOADED=2、STOPPING=3、INTERNAL_ERROR=4、
DEADLINE_EXCEEDED=5、RESOURCE_LIMIT=6。硬限触发替代错误时只统计实际成功入队的替代响应。
坏头或截帧可直接关闭，因此协议错误不一定有错误响应；响应入队后也可能因RST或期限只发送部分字节。

搜索和推荐各自的六桶为`[0,100]`、`(100,1000]`、`(1000,10000]`、`(10000,100000]`、
`(100000,1000000]`和`(1000000,+∞)`微秒，每次仅进一个桶；这些不是累积Prometheus桶。
服务耗时包括缓存/计算及服务异常映射，不含队列等待、结果发布、日志解析和网络I/O；失败也记延迟。
未饱和时桶和等于count，所有累计字段到uint64最大值饱和，不回绕。

## 一致性与限制

Connection由所属Sub独占计量，Sub发布值报告；查询两组聚合在同一小锁下更新/复制，
缓存、worker及各队列使用自己的锁。锁不跨execute、网络或日志输出。
不同组件采样时点不同，不是全局原子快照；运行期不能用跨组件等式或共享计数差精确归因。
关闭屏障完成后数值稳定，可核算最终总量。high_bytes不是缓冲capacity、全局同时峰值或RSS，
shutdown耗时不是网络deadline，也不是退出硬上限。

真实CTest入口`metrics.contract`验证端点、饱和、8线程聚合、部分发送、分配/同步异常与行界限；
`metrics.production`用原创快照与独立网络编码核周期/最终样本、字节和业务计数、过滤及坏输出；
`net.worker_saturation_1/2`在确定门控下核busy和等待深度。这些检查不证明容量、无阻塞或生产性能。
