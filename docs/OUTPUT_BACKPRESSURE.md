# 输出高低水位与背压

Connection 的输出表示尚未交给本机内核的完整线字节，包含24字节头。`OutputWatermarks`
定义低水位、高水位和硬限额；仅所属I/O线程修改相关状态，worker不接触Connection。
接口合同见[Connection](../include/siftwing/net/connection.hpp)和
[ProtocolSubReactor](../include/siftwing/net/protocol_sub_reactor.hpp)。

## 配置与滞回

`low_bytes < high_bytes <= hard_bytes <= 24+max_body_bytes`，low允许0，hard至少26字节，
以容纳最小PONG。三个值全0表示自动值：hard=`24+max_body_bytes`、high=`hard*3/4`、
low=`hard/2`（整数取整）。其他组合必须完整有效；非法配置在接管fd或启动线程前拒绝。
旧三参Connection构造保留，新增四参重载显式传入水位。Sub通过`config.output`选择，
OnlineServer沿`config.sub.output`传递；生产命令当前使用自动值，没有新增命令行选项。

例如协议Body上限512，配置`{32,64,96}`：输出达到64字节进入暂停；发送后剩余63或33
仍暂停；剩余32时解除输出暂停原因。`output_backpressured()`可查询滞回状态，
`output_watermarks()`返回解析后固定配置。只有真实send消费字节才能降低水位；EAGAIN、
无效预算或仅收到完成通知不降低水位。关闭/发送失败清空输出及暂停状态。

输出Buffer本身以hard为容量上限，默认仍容纳原有最大一帧。该上限不包含调用方Frame、
JSON验证临时分配、worker结果、输入Buffer、内核socket缓存，也不是全进程RSS硬限。

## 与单连接在途门禁协作

[单在途合同](SINGLE_INFLIGHT.md)仍更严格：输出未排空时，即使低水位已解除输出暂停，
也不能交付下一帧、继续recv或提前finish。Sub保留OUT，等待worker时移除IN/RDHUP；
只有drained后才finish，主动继续已缓存请求，并在需要时恢复IN/RDHUP。
因此水位不引入并行请求、第二个响应队列或提前读取；低位解除与请求完成是两个条件。
半关闭仍保留完整请求，错误、PING和业务响应共用此规则；其他连接继续独立服务。

## 拒绝与局部关闭

`queue_response`先检查已关闭/无请求/重复响应/ID与成功类型，再检查codec，最后检查硬限。
合法完整响应超过hard返回`QueueStop::output_limit`，不编码完整线副本、不复制、不send；
响应源、fd、输入、输出和请求门禁保持，调用方可换成更小的同ID错误响应。
恰好hard允许，协议Body上限仍独立有效；没有截断或静默丢弃响应后缀。

正式Sub对直接响应和异步完成使用同一策略：第一次硬限拒绝后，尝试同请求ID的固定
`RESOURCE_LIMIT`（status=6、`{"message":"resource limit"}`）。替代错误仍须符合原协议
Body与输出硬限；无法容纳或构造异常时仅关闭该连接，不递归构造错误，不停止整个Sub。
错ID/类型、重复或陈旧结果仍按原关联门禁拒绝，不触发容量替代，也不解除正确请求。

`output_limit_rejections`计首次合法响应的硬限拒绝，`output_limit_closes`计随后固定错误
也无法构造/入队的关闭。`completion_applied`包含已复制的替代错误；它不证明原成功结果
被发送，也不证明对端收到。原任务队列full/closed仍沿OVERLOADED/STOPPING，若这些
固定错误超输出硬限也采用上述局部策略。结果邮箱full等服务级安全兜底保持原合同。

慢读者可占用一个有界连接和一个输出；暂停读取将压力沿内核流控传回发送方。当前没有
慢读期限或按持续时间驱逐，停止仍立即丢弃。连接/队列条目容量和每轮I/O预算沿原组件；
不能把这些局部界限解释为统一的全局字节准入、吞吐保证或任意调度公平性。

现有配置还给出数量界：等待任务不超过task_capacity，执行中的任务不超过workers；
每连接最多一个pending，连接总数不超过sub_count * sub.max_connections，每Sub结果邮箱
条目不超过completion_capacity。已完成而等待发送的响应与worker运行任务分别计量，
这些数量界不构成跨阶段统一的全局字节计费或准入。

## 验证入口

三个真实CTest为`net.output_backpressure_limits`、`net.output_backpressure_sub`、
`net.output_backpressure_tcp`，Release也执行检查。独立黄金线字节验证high-1/high/high+1、
hard/hard+1、low+1/low、配置拒绝时fd保持、错误优先级、拒绝后替代和真实EPIPE清理。
本地stream专项以真实前缀send/确定EAGAIN证明低位不提前续读、缓存/同ID/SHUT_WR顺序，
并覆盖异步结果、Body拒绝、full、closed的局部关闭。真实TCP专项缩小socket缓存并暂不读，
观察真实EAGAIN及另一连接的PING进展，随后核完整后缀、缓存PING及EOF。
这些验证不证明公网拥塞、持续慢读期限、性能或所有调度下的行为。
