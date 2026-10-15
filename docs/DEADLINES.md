# 单调定时器与期限容器

[DeadlineQueue](../include/siftwing/net/deadline_queue.hpp)维护固定容量期限，
[timerfd原语](../include/siftwing/net/deadline_timer.hpp)提供Linux定时唤醒。
正式在线服务的每个`ProtocolSubReactor`在所属线程创建一个非阻塞、CLOEXEC的
`CLOCK_MONOTONIC` timerfd，并在同线程注册、读取、注销和关闭。当前初始保持disarmed，
尚无空闲超时或请求deadline策略，也未改变已有请求、半关闭或立即停止行为。
旧空`SubReactor`生命周期基线和单Loop probe保持原职责。

期限是`CLOCK_MONOTONIC`纪元起的正`int64`纳秒，不是墙钟、Unix时间、相对时长或线上字段。
该时钟不受墙钟校时影响，Linux下不包含系统挂起时间；不假定C++ `steady_clock`与它同纪元。
`monotonic_now()`读同一时钟并检查换算溢出；调用者自行检查加时长的溢出。
期限等于当前时间即到期，过去的正期限可立即唤醒；0不能表示期限，空`optional`才表示disarm。

容器构造容量为1..2048，预分配后不再分配；同key更新只占一个条目，真实取消立即释放容量。
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
本模块没有为未来策略预埋连接deadline字段；泛型容器与设时的组合由专项真实epoll闭环验证。

真实CTest：`net.deadline_queue`用独立map参考验证集合行为；`net.deadline_timer`验证
真实绝对设时、提前/推迟最小期限、取消、同期限多个条目与一个内核通知、fd 0及有限读取；
`net.sub_timer`验证两个正式Sub、真实TCP PING、单timer归属、真实到期读取和创建/注册/
读取/清理失败。故障通过测试静态链接GNU符号包装注入，没有增加生产测试接口。
这些测试不证明任意调度、延迟上界、系统挂起行为或尚未接入的连接超时策略。
