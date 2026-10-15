# 有界连续字节 Buffer

`siftwing::net::Buffer` 位于 Connection 的 socket 读写与 codec 之间，拥有字节数组，不解释
协议、不调用系统 I/O。接口见 [buffer.hpp](../include/siftwing/net/buffer.hpp)，
实现见 [buffer.cc](../src/net/buffer.cc)，行为验证见
[buffer_test.cc](../tests/unit/buffer_test.cc)。CMake 目标为 `siftwing_buffer`，
CTest 名称为 `net.buffer`。已由[增量解码](FRAME_DECODER.md)、[Connection](CONNECTION_READ.md)及[在线服务](ONLINE_SEARCH.md)组合使用；组件自身不执行网络 I/O。

构造时明确 `max_bytes`，可选初始容量，默认延迟分配。初始容量超过上限抛
`std::invalid_argument`；零上限合法，只允许空数据。对象独占存储，禁止复制和移动，
由调用者串行使用。输入与输出可以各自使用一个 Buffer，上限由调用方分别选择；
协议最大总帧长度 1,048,600 字节不是所有 Buffer 必须采用的预算。

内部不变量是 `0 <= read <= write <= capacity <= max_bytes`。可读数据连续，
`readable_bytes()` 返回未消费长度，`writable_bytes()` 只表示尾部空闲空间。
`readable_view()` 借用可读区，支持 NUL 等任意字节，不保证 NUL 终止。

`append(view)` 要么追加全部字节，要么因超限返回 `false`，超限时不读输入、不改变
数据、容量或索引。空追加成功。尾部够用时直接写入；总空间够用但尾部不足时压缩；
否则按倍增并饱和到上限的方式增长。零容量首次分配不超过 1024 字节。支持追加自身
可读区的子视图，包括发生压缩或增长的情况。输入必须在调用期间有效。

`consume(n)` 仅消费前缀，越界返回 `false` 且保持状态；全部消费后索引归零。
`clear()` 丢弃全部可读字节并归零索引；二者都保留数组容量，不擦除历史数据。
每次成功非空追加、非零消费或 clear 后应重新获取视图；失败和空操作保持旧视图有效。
codec 必须等完整数据可用后再按其合同消费，不能凭 Buffer 存在推断帧完整。

分配异常直接传播，异常前保留原数据、地址、容量和索引。压缩会先保存输入副本以处理
自追加；增长先填充新数组，再提交所有权。上限约束拥有的数组与未消费数据，不能代表
RSS 硬上限：增长期间新旧数组共存，压缩还可能拥有临时输入副本。

连续数组便于 codec 读取帧头，代价是压缩时搬移字节。环形或分段结构能减少搬移，
但要求解析与发送处理分段视图。当前先采用连续方案；这里没有性能结论或动态缩容承诺。
