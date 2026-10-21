# 空闲超时、请求deadline与单调定时基础

[DeadlineQueue](../include/siftwing/net/deadline_queue.hpp)维护固定容量期限，
[timerfd原语](../include/siftwing/net/deadline_timer.hpp)提供Linux定时唤醒。
正式在线服务的唯一Reactor在应用调用线程创建一个非阻塞、CLOEXEC的
`CLOCK_MONOTONIC` timerfd，并在同线程注册、读取、注销和关闭。初始disarmed，
可按固定配置启用空闲超时和请求deadline；两项默认关闭，旧立即停止策略保持；[优雅退出](GRACEFUL_SHUTDOWN.md)复用同一timerfd并取原期限与公共排空期限的最早值。
生产入口可用`--idle-timeout-ms`/`--request-timeout-ms`或[启动配置](SERVER_CONFIG.md)启用，启动后固定。
旧独立ProtocolSubReactor/空Sub及单Loop probe暂留原组件测试；生产没有Sub线程。

期限是`CLOCK_MONOTONIC`纪元起的正`int64`纳秒，不是墙钟、Unix时间、相对时长或线上字段。
该时钟不受墙钟校时影响，Linux下不包含系统挂起时间；不假定C++ `steady_clock`与它同纪元。
`monotonic_now()`读同一时钟并检查换算溢出；调用者自行检查加时长的溢出。
期限等于当前时间即到期，过去的正期限可立即唤醒；0不能表示期限，空`optional`才表示disarm。

容器构造容量为1..65536，预分配后不再分配；同key更新只占一个条目，真实取消立即释放容量。
满时拒绝新key但仍可更新已有key，非法输入与full均保持原集合。每次按`(when,key)`
取最早值，只有`when <= now`才删除并返回；相同期限按key升序。所有操作由所属线程串行调用，
O(capacity)有界扫描；它不拥有连接、回调或锁，不证明key代表的连接/请求仍有效。
调用者负责身份验证、key生命周期、预算及实际到期动作。

组合使用的顺序是：

1. 更新/取消容器，取`next()`，用其期限重设一次绝对timerfd；空集合显式disarm。
2. epoll就绪后有限drain，重新读取同钟时间，按预算从容器取所有已到期条目。
3. 仍有到期条目时安排下一轮继续，再按当前最小期限重设。不得按timerfd计数pop。

容器变化与内核设时是两个阶段；设时失败不撤销已完成的容器操作，调用者必须处理诊断，
不能带着未同步的定时器继续宣称期限有效。创建、设时各单次尝试，含EINTR均如实返回；
读取的成功与EINTR均占有限预算，EAGAIN才表示本次观察暂时无计数，短读/EOF为EIO。
不提供硬实时响应、任务取消或线程安全承诺。

timerfd和eventfd都使用控制token 0，控制就绪时清timer、下一轮清eventfd；非零连接token
空间完整保留。重设/取消可以清掉内核旧计数，已取得的epoll批次仍可能含陈旧就绪，读到
EAGAIN是正常情况。生产资源失败沿现有Sub首因停止/启动回滚处理，清理诊断独立保留。
正式Sub用真实连接状态驱动固定容器，生命周期策略和即时门禁见下文。

真实CTest：`net.deadline_queue`用独立map参考验证集合行为；`net.deadline_timer`验证
真实绝对设时、提前/推迟最小期限、取消、同期限多个条目与一个内核通知、fd 0及有限读取；
`net.sub_timer`验证两个正式Sub、真实TCP PING、单timer归属、真实到期读取和创建/注册/
读取/清理失败。故障通过测试静态链接GNU符号包装注入，没有增加生产测试接口。
基础测试证明范围仅限上述机制；连接策略由下列独立专项证明，不外推任意调度或延迟上界。

## 两类时间语义

`ProtocolSubConfig`末尾新增`idle_timeout_ms`和`request_timeout_ms`，均为有符号毫秒，
范围0..86400000（一天）；0关闭该策略，负数或超范围在任何线程启动前抛`invalid_argument`。
`OnlineServerConfig::sub`原样传递；命令行默认关闭，可用`--idle-timeout-ms`/`--request-timeout-ms`或[启动配置](SERVER_CONFIG.md)启用。
时长转纳秒及与当前时钟相加均检查边界，溢出不截断或回绕。

| 状态 | 起点和刷新条件 | 终点与到期行为 |
|---|---|---|
| 无在途请求：idle | 接纳连接、上个输出排空后起算；实际recv取得正字节才刷新 | 完整受信帧交付时替换为request；到期关连接 |
| 有在途请求：request | 完整受信帧交付起算，含PING/Body拒绝/任务拒绝；进展和completion不延长 | 输出全部交本机内核并finish时取消；到期关连接 |

空闲计时在pending期间暂停；即使仅idle启用，也不会用idle期限关闭正在等待worker或发送输出的连接。
部分输入不启动request计时；缓存解析、EAGAIN、控制唤醒不刷新idle。SHUT_WR不撤完整请求，
仍受request期限约束；响应排空后继续缓存和EOF观察。request_id完成后仍可复用。

期限满足`when <= now`即到期。Sub在wait前后扫描实际集合，每次服务前、发送前后以及
completion应用前独立判时，因此不依赖timer与结果/I/O在epoll批次中的先后。
非阻塞send和有限预算仍可跨过期限；发送后检查发现到期时不宣告及时finish，
但已经交给内核的字节无法撤回。此设施不抢占系统调用、回调或worker，不提供硬实时延迟保证。

到期先真实删除条目、撤销完整ConnectionId，再注销/关闭连接；不追加TIMEOUT ERROR。
已发送的部分响应可能被丢弃后缀，客户端须按帧完整性和断连处理。若追加错误并保留连接，
还需处理部分帧及请求代次的额外合同，当前不采用。已提交worker继续执行或排队，
结果由所属Sub按身份/期限门禁拒绝；连接关闭后同slot下一代不会接收旧结果。
仍要求每accepted任务至多发布一次，不能识别违反此合同且跨合法同ID复用的旧副本。

## 所有权、预算与失败

每个Sessions预分配一个容量为`max_connections`的`DeadlineQueue`，每slot最多一个idle或request条目。
key为slot下标+1；重设同key原位更新，drop真实删除后才复用slot，不积累旧期限，也不持任务/连接指针。
每轮最多取`max_connections`个到期条目，再按当前最小期限arm；空集合disarm。
单次容器操作O(capacity)，一轮清理最坏O(capacity²)，当前连接上限1024，存储和工作量均有界，
没有性能或任意调度公平性声明。两项关闭时不读时钟或额外调用settime，timer资源仍按基础合同存在。

`idle_timeouts`、`request_timeouts`只计真实到期关闭；局部`first_connection_error`记录ETIMEDOUT，
不自动停止服务。时钟、时间换算、容器不变量或设时失败是定时基础设施首因，停止所属Sub并由
正式OnlineServer协调停机，沿原发布者静止屏障清理；不会把基础故障伪装成普通客户端超时。
结果received仍按applied/stale/rejected精确分类；应用后发送失败不重复记为rejected。

## 策略验证入口

`net.sub_deadlines`使用真实Sub和流socket，静态链接包装时钟给出独立绝对边界：部分帧刷新、
完整帧起点、严格早于/等于期限、completion先于到期扫描、send跨期限、pending暂停idle、
排空后重启idle、半关闭、真实EAGAIN、slot复用和同期限多条目，以及时钟/设时失败和加法溢出。
另用不包装时间的真实TCP双Sub验证timer主动关闭空闲连接。
`net.online_deadlines_1/2`用公开原创夹具的真实SearchService、WorkerPool、TCP和一/双Sub，
暂停首个真实execute，证明执行中与排队请求均到期、健康PING推进、worker未被取消、
迟到结果被丢弃，以及新连接复用request_id后业务和缓存PING正常完成。
GNU包装仅用于测试，不增加生产测试接口；真实查询仍执行。

这些验证不证明公网慢网、系统挂起、真实OOM、任意调度、性能、任务取消或优雅排空。
