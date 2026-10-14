# Main-Sub Reactor 线程拓扑与所有权合同

本文冻结职责、资源转交和生命周期边界。当前可执行网络命令仍是
[单循环协议探针](PROTOCOL_LOOP.md)；已有独立线程的[Sub生命周期](SUB_REACTOR.md)，
已有独立[fd移交队列](FD_HANDOFF_QUEUE.md)、[eventfd唤醒组件](EVENTFD_WAKEUP.md)和[Main轮询接入组件](MAIN_REACTOR.md)；
[多Sub协议集成](MULTI_REACTOR.md)以独立ProtocolSubReactor组合Main，验证固定归属与停止屏障；旧空Sub保持，独立[WorkerPool](WORKER_POOL.md)及[SearchTask/Completion载荷](SEARCH_TASK.md)已实现，独立[CompletionQueue](COMPLETION_QUEUE.md)已提供结果邮箱；worker接入协议链和正式搜索服务命令仍待后续。
本拓扑合同不规定具体 C++ API，也不改变[协议](PROTOCOL.md)；已实现接口见对应组件文档。

## 线程与职责

目标拓扑包含1个Main I/O线程、启动时配置的N个Sub I/O线程（N为正，默认2）和独立worker池。
[有界业务线程池](WORKER_POOL.md)已提供固定1—64线程；正式业务接入仍待后续，
不把硬件并发数当作必然可用线程数。每个 Reactor 使用自己的 LT epoll 和 wakeup eventfd；
ET、动态扩缩容、连接迁移和无锁队列留给有证据的后续扩展。

| 角色 | 所属线程内的职责与资源 | 允许的跨线程入口 | 禁止的动作 |
|---|---|---|---|
| 生命周期控制者 | 启动配置、线程拥有者、就绪/失败汇总、停止编排、join | 同步后的启动结果、停止请求、错误报告 | 直接读写/关闭活跃 Sub 的连接或 epoll；持有目标线程需要的锁时join |
| Main | Listener、监听 epoll、accept4、新 socket 配置、轮询选择、停止接入 | 有界 fd 移交、通知目标 Sub | recv/send 客户端业务数据、JSON 解析、查询/分词/文件 I/O |
| Sub | 自己的 epoll/wakeup、连接表、Connection、Buffer、注册和写偏移 | 消费自己的 fd/Completion 队列；非阻塞提交业务任务 | 访问其他 Sub 的连接、跨 Sub 迁移连接、等待业务队列空位 |
| worker（业务接入后续） | 拥有型任务、查询/推荐/摘要/缓存/响应序列化；只读索引 | 向目标 Sub 的有界队列投递 Completion，再通知 | 持有 Connection 指针或客户端 fd；直接 epoll_ctl/recv/send |

生命周期控制者是职责，不额外要求一个常驻线程；可由应用调用线程承担。Main 的全局停止
控制负责停止接入并协调报告，不能绕过所属线程清理资源。所有 join 由线程拥有者在目标
线程之外执行；禁止 self-join、detach 代替清理或让仍可执行的线程借用已析构对象。

```mermaid
flowchart LR
    Peer[TCP 客户端] --> Main[Main：accept 与轮询]
    Main -->|成功入队：转交 fd| Queue[目标 Sub 的有界 fd 队列]
    Main -.->|eventfd：提示检查队列| Sub[Sub：注册与连接 I/O]
    Queue -->|所属线程取出| Sub
    Sub <-->|recv / send| Peer
    Sub -->|拥有型任务；后续| Workers[独立 worker 池]
    Workers -->|拥有型 Completion；后续| Results[目标 Sub 的有界结果队列]
    Results --> Sub
```

数据由队列持有，eventfd 只提示队列可能有工作；通知次数、任务数和连接数不相等。
Main/Sub的队列与通知协议链已有集成验证；图中worker与Sub的业务接入、Completion回投和搜索业务路径仍是计划；独立WorkerPool及请求/响应拥有载荷已有组件验证。

## fd 转交：每个时点只有一个拥有者

客户端 fd 使用已有 [UniqueFd](SOCKET_BASELINE.md) 的移动所有权。get() 的整数仅为同步
借用，不能跨线程保存来关闭资源；std::move 也不建立线程间同步。第一版采用有锁有界
队列，成功发布与消费通过同一同步协议建立 happens-before。

| 时点或结果 | 唯一拥有者 | 必须发生的动作 |
|---|---|---|
| accept4 成功，尚未发布 | Main 的局部 UniqueFd | 有限配置并选目标；失败由 Main 关闭一次 |
| 成功入队的提交点 | 目标 Sub 管理的队列条目 | Main 的源拥有者为空，不再读写、关闭或重新投递该 fd |
| 队列满、停止、非法目标，或提交点之前分配失败 | Main，原拥有者保持 | 不阻塞等空位；Main 关闭该新 fd，不自动改投其他 Sub |
| 入队成功，随后唤醒失败 | 目标 Sub 的队列 | 保留已转交事实；报告通知错误并进入受控停止，Main 不能取回或关闭该数字 |
| Sub 取出条目 | Sub 的局部 UniqueFd，随后 Connection | 在所属线程构造、注册；注册成功才纳入连接表 |
| 构造或注册失败 | Sub 的局部拥有者 | 关闭一次，记录本地错误；资源性/系统性失败按生命周期合同停止目标并报告 |
| 活跃连接关闭 | 所属 Sub 的 Connection | 对仍有效注册先 DEL 再 close；已自行终止则不 DEL 旧数字 |
| 停止时尚在队列中的 fd | 目标 Sub 的队列 | 在所属线程 drain 并关闭；线程退出前不得遗留队列拥有的 fd |

这里“队列属于 Sub”表示目标归属与生命周期，生产者只能使用同步后的发布入口。
提交与关闭入口必须有一个确定的先后点：先提交成功则 Sub 负责清理，先关闭则拒绝且
Main 保持拥有。不能出现“返回失败但源已被消费”，也不能用通知写入成功来定义队列提交。
fd队列已用FdHandoffResult区分accepted与closed/invalid_fd/full，公共合同见
[队列说明](FD_HANDOFF_QUEUE.md)；通知结果已有[eventfd组合接口](EVENTFD_WAKEUP.md)，区分提交与通知两个阶段。

轮询只选择就绪且接入入口开放的固定 Sub，游标按每次选定目标的移交尝试前进，
满队列不在同一尝试中重新分配；[Main组件](MAIN_REACTOR.md)与[多Sub协议组合](MULTI_REACTOR.md)已覆盖此分配边界；正式搜索服务仍待后续。该合同不保证
相等连接数、相等负载或均衡 CPU。运行时 Sub 退出按服务失败协调停止，不静默缩容。

## 连接、队列与索引的生命周期

Connection 从注册到终止固定归属一个 Sub，保持已有“不复制、不移动、所属线程串行”
合同；必要时用 unique_ptr 保持地址稳定。Main 不保留 Connection 引用，worker 只带拥有
的输入和结果。当前 Loop 的单调 token 只是注册身份；跨线程结果的[ConnectionId/generation与路由包装](CONNECTION_IDENTITY.md)已提供独立组件并迁移Sub注册元数据；
request_id/deadline校验及过期结果应用仍由后续回投模块完成，不用fd数字代替。

队列状态、停止标志与错误报告是跨线程同步对象；Connection/Buffer/epoll 状态是所属
线程对象。const、volatile、unique_ptr 或“只写一次”都不自动提供同步。队列锁内只做
有限状态与条目操作；关闭 fd、注册、回调、业务计算和 join 在释放锁后进行。

跨线程通知端点的借用也受生命期门禁保护。销毁或关闭前必须关闭发布入口，并确认
所有已进入的发布/通知调用已经返回；不能仅凭队列已空或 Main 已停止 accept 就关闭
eventfd。运行失败同样先撤销入口并协调生产者静止，再清理，不让旧借用写入复用 fd。

只读索引可以由任务保留共同生命期，但 shared_ptr 只保护拥有关系，不保护可变对象。
未来 worker 使用的分析器、缓存及业务辅助对象必须另证线程安全或采用每 worker 独立
实例；本合同不把现有 const API 扩大为可并发调用。索引热切换仍属于后续版本。

## 启动、失败与停止

1. 控制者完整预检配置，建立固定 Sub 拓扑；线程创建、对象分配和就绪报告都有失败出口。
2. 每个 Sub 在自己的线程内建立并清理 I/O 资源，发布完整就绪或首个失败；启动中的 Sub
   不接收 fd。控制者不能仅凭线程已创建就视为就绪。
3. 全部 Sub 就绪后才开放 Main 接入。后续若含 worker，也须先完成其启动门禁。
   Main 的监听创建/注册失败仍须请求停止并 join 已启动 Sub，不能发布半个服务。
4. 部分启动失败时停止已启动线程、让其所属线程清理，然后逐个 join；未创建的线程
   不 join，线程函数边界吸收异常并报告首因。清理错误另记，不覆盖原失败。
5. 停止请求幂等，先关闭全局接入，再让 Main 停止 accept，关闭 fd 发布入口，阻止新请求
   和新业务任务；与提交并发的 fd 按上述提交点决定清理方。Main 确认已进入的 fd 发布与
   通知调用全部返回，控制者完成停止通知后也撤销该借用；Sub 清理前须取得生产者静止确认。
6. 业务引入前，允许立即丢弃模式：Sub 关闭待接入 fd 和连接，在所属线程移除并关闭
   wakeup/epoll，再退出；控制者 join 后才能销毁队列、通知端点和线程借用的配置。
7. 业务引入后，须先完成/取消在途任务并停止、join worker，再让 Sub 收口 Completion
   和输出，最后清理并 join Reactor。worker 可能回投时，接收队列及通知端点保持有效。
   具体截止时间、结果排空、丢弃规则和线上状态由优雅退出模块实现，本模块不承诺交付。

跨线程停止必须有可验证的唤醒或有限等待后备路径，不允许无限 epoll_wait 配合仅写一个
停止标志。eventfd 接入前可由生命周期模块采用有限等待过渡；eventfd 接入后的通知失败
也必须能在有限时间内观察停止。超时数值和具体机制由实现模块冻结，不从“调用了 stop”
推断线程已经退出。停止请求返回、线程退出和 join 完成是三个不同事件。

## 对当前实现的影响与后续验收

现有 ProtocolLoop 独占 Listener、epoll 和连接，保留为单循环协议基线；不能把它直接放到
多个线程并共享调用，也不能让每个 Sub 通过它重新监听端口。ProtocolSubReactor已组合已有 Connection、
codec、socket 与 epoll 组件，Main 组合 Listener；共享算法需要提取时先保持旧行为并验证。
本合同不改变旧 API、所有权、线格式、快照格式或 CLI，也不引入通用回调框架。

| 实现模块 | 本合同要求其证明的行为 |
|---|---|
| Sub 生命周期 | 全就绪门禁、部分启动回滚、重复停止、所属线程资源清理、join 与异常出口 |
| fd 有界队列 | 成功消费源；满/关闭/失败保留源；关闭竞态有提交点；遗留条目关闭一次 |
| 新连接 eventfd | 队列与通知分离、合并/drain、EINTR/EAGAIN、成功提交后通知失败、发布者静止、停止可达 |
| Main 轮询与多 Sub 集成 | Main 不读写客户端；固定归属；满队列拒绝；失败协调停止；旧单 Loop 保持 |
| 有界 TaskQueue | [独立组件](TASK_QUEUE.md)已提供拥有任务移交、容量拒绝、消费者唤醒与关闭排空；[WorkerPool](WORKER_POOL.md)已提供固定线程执行与关闭排空；业务接入仍待后续 |
| worker 与异步身份 | 无裸 Connection 指针/fd、独立数据拥有、generation 校验、过期 Completion 拒绝 |
| 优雅退出 | worker 回投端点生命期、完成/取消/排空顺序、截止时间与无线程/fd 泄漏 |

M2.12 的验证仅为合同审读与文档检查；Sub生命周期验证见[组件说明](SUB_REACTOR.md)，
独立fd队列的验证入口见[队列说明](FD_HANDOFF_QUEUE.md)。
Main/Sub 连接与协议组合验证见[多Sub集成](MULTI_REACTOR.md)；吞吐、延迟、公平性和
独立worker生命周期见[WorkerPool](WORKER_POOL.md)，业务接入/优雅退出仍待后续验证，不能以本文件存在或空 Sub 生命周期测试通过替代。

独立eventfd唤醒协议的真实epoll验证与稳定借用停止门禁见[eventfd说明](EVENTFD_WAKEUP.md)。
