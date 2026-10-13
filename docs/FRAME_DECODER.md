# 增量帧解码

当前内存解码器按[版本 1 线合同](PROTOCOL.md)从[Buffer](BUFFER.md)取出完整帧；
不依赖 socket，也不判断一次接收等于一帧。公共接口权威在
[frame_decoder.hpp](../include/siftwing/net/frame_decoder.hpp)，实现见
[frame_decoder.cc](../src/net/frame_decoder.cc)。

## 调用与结果

调用者将收到的字节追加到自己拥有的 Buffer，再串行调用
`decode_next(input, role, max_body_bytes)`。默认 Body 上限为 1048576 字节；
本地配置必须在 `[2,1048576]`，接收角色必须为 `server` 或 `client`。
配置错误抛出 `std::invalid_argument`，不改变输入。

| DecodeResult 分支 | 含义与调用方动作 | Buffer 变化 |
|---|---|---|
| `std::monostate` | 帧头不足 24 字节，或合法头的 Body 尚不完整；等待后续输入 | 完全保持 |
| `Frame` | 一个完整的原始帧；继续验证 Body，或按需要再取下一帧 | 恰好消费首帧，保留后续字节 |
| `DecodeError` | 非法头、方向或无法容纳的帧；终止该字节流 | 完全保持，不搜索 magic 重同步 |

解码器不另存半帧状态。半帧的唯一字节所有者是 Buffer，调用者必须在等待或拒绝时
停止取帧循环。多帧粘连会按输入顺序逐次返回；完整帧后的半帧保持至下次追加。
Body 复制完成、结果构造成功后才消费；分配异常直接传播，原字节、索引、容量与 view
保持，可在资源允许时重试。返回 Frame 拥有 `std::string`，Buffer 复用或销毁后仍有效；
成功消费后须重新获取 Buffer 的 view。同一个 Buffer 的读写由调用者串行化。

## 验证边界

满 24 字节后依次检查 magic、version、header_size、已知 message_type、flags、
status 与类型组合、Body 长度和本地 Body 上限、request_id、接收方向。
多字节字段逐个八位字节读取为大端整数，无对齐、别名或主机端序前提。
长度验证先于总长加法、Body 等待与分配，`0xffffffff` 立即拒绝。
合法协议头之后再检查 `24 + body_length <= input.max_bytes()`，不满足返回
`buffer_limit`，避免等待一个给定 Buffer 无法容纳的帧。Buffer 上限不等于 RSS 上限，
复制 Body 期间输入数组与结果字符串共存。

Frame 只表示头部验证及字节完整。Body 保留原字节，本模块不验证 UTF-8、JSON 对象、
PING/PONG 的 `{}` 或业务字段；它不能直接视为有效请求。`DecodeError` 是本地拒绝原因，
Frame 的 status 是线上状态，二者不可互换。编码、JSON/业务处理与错误响应仍由后续模块
实现，不据非法头猜测 request_id 并发送响应。

当前没有 EOF 参数。上层收到 EOF 时，非空未完成帧应判为截断并终止，不能发布部分 Body。
本模块没有 recv/send、连接状态、在途请求控制、Reactor、线程池或背压实现；内存粘连测试
不证明 V2.0 允许业务流水线，也不证明真实 TCP 行为。

## 行为验证入口

真实 CTest `net.frame_decoder` 使用手工黄金字节和独立字段构造，覆盖全部两段切割位置、
逐字节输入、多帧与所有末尾前缀、不同分块下的 Buffer 复用、九种类型/角色/status、
大端高位关联号、最大 Body、本地上限、非法头拒绝顺序和定点分配失败保持。
这是本层的行为验证；更广黄金/fuzz 边界、编码与网络验证尚待后续模块。
