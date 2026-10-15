# 单连接一个在途请求

V2当前每个连接最多处理一个在途请求。此限制从Connection交付完整受信帧开始，
覆盖提交、等待worker、结果回投、响应复制和部分发送，直到响应全部交给本机内核。
PING、受信Body拒绝、任务拒绝和业务错误使用同一门禁；不同连接可独立推进。

```text
读取并交付一帧 → 等待结果或直接生成响应 → 输出未排空 → 排空并finish → 继续下一帧
                    暂停读事件              只监听写事件
```

TCP可以一次送来多帧。Connection可能已缓存后续字节，但在途期间不继续recv、解码或
交付下一帧。后续请求按流顺序保留，不因“已经有一个请求”而自动回复OVERLOADED；
这也不表示服务器已接纳后续任务。输出排空后Sub主动继续缓存，不依赖新的EPOLLIN。
无应用缓存时恢复读事件处理内核余量。单连接串行不限制多个连接之间的worker并发。

等待结果时Sub保留Connection pending，移除IN/RDHUP意向；复制响应后保留门禁并只
监听OUT。短写、EAGAIN及发送预算耗尽保留精确后缀；wrong ID、wrong成功类型和重复
completion不能使当前请求完成。公共合同见[Connection](../include/siftwing/net/connection.hpp)
和[ProtocolSubReactor](../include/siftwing/net/protocol_sub_reactor.hpp)。

`shutdown(fd, SHUT_WR)`表示对端不再发送，但仍能接收响应。完整请求和此前缓存的帧继续
处理，最后才读取EOF；完整帧后的残缺尾帧沿既有截断合同关闭。ERR/HUP、I/O失败或当前
立即停止策略仍可丢弃连接，不承诺所有请求必达。输出排空只证明本机内核接纳字节，
不证明对端接收或处理；TCP发送方向结束与整个连接失效不是同一事件。

request_id是关联号，允许前一请求完成后再次使用，既不要求单调也不作全局去重。
连接generation用于防止旧连接结果误投，不能区分同连接复用相同ID的违规旧副本；
每个accepted任务至多发布一个结果仍是调用者合同，没有新增请求epoch或取消语义。
底层`finish_request()`仍由调用者确认逻辑完成，不能刚提交任务就提前调用；正式Sub
只在`flush_output()`返回drained后调用，不改变其他底层调用者的既有用法。

三项CTest为`net.single_inflight_worker`、`net.single_inflight_output`和
`net.single_inflight_errors`。真实Sub/WorkerPool配合同步暂停、静态recv/epoll观察和
精确部分send/EAGAIN，按独立黄金线字节检查门禁、其他连接推进、缓存/内核余量续读、
重复ID、错误/拒绝与SHUT_WR。该专项使用本地流socketpair；真实TCP业务沿[在线搜索测试](ONLINE_SEARCH.md)。
注入不模拟TCP拥塞、真实慢网或任意调度，不证明性能。输出高低水位/硬限额与局部拒绝见[输出背压](OUTPUT_BACKPRESSURE.md)；低位不解除在途门禁。全局过载、deadline、
取消、多在途、缓存及优雅退出各自留待对应模块。
