# 帧编码、Body 准入与统一响应

`siftwing_frame_codec` 在[增量拆帧](FRAME_DECODER.md)之后独立验证 Body，并按
[版本1线格式](PROTOCOL.md)生成完整帧。公共合同在
[frame_codec.hpp](../include/siftwing/net/frame_codec.hpp)；公共接口不暴露第三方类型。
已有内存接口与行为测试；socket、实际业务执行、收发调度和 EOF 接线尚未实现。

## 接口与所有权

| 接口 | 成功 | 拒绝 |
|---|---|---|
| `validate_frame(frame, receiver, limit)` | `nullopt`，输入保持 | `ProtocolError` |
| `encode_frame(frame, receiver, limit)` | 拥有完整线字节的 `string` | `ProtocolError`，无部分字节 |
| `make_response(request, body, limit)` | 拥有对应成功响应的 `Frame`，status=0，原ID | `ProtocolError` |
| `make_error_response(request, status, limit)` | 拥有 ERROR_RESPONSE、指定状态、原ID | `ProtocolError` |

`receiver` 是接收这些字节的对端角色：请求用 `server`，响应用 `client`。所有接口使用 `[2,1048576]` 的 Body 上限，端点合法；非法配置/角色抛
`invalid_argument`，分配异常传播。所有输入仅在调用内同步借用，须保持稳定且不能并发
写入；失败/异常不修改输入，成功结果与输入生命独立。预算不是进程 RSS/CPU 硬限制。

## 检查顺序与 Body 边界

内存 Frame 先检查已知类型→status组合→实际Body长度→非零ID→接收方向，随后检查
严格UTF-8→JSON对象→心跳。线上 magic/version/header_size/flags 固定由编码器生成；
解码器继续按已冻结顺序拒绝非法线头，不能借这组 Frame API 接纳未知协议版本。

Body 准入采用固定版本 nlohmann/json SAX（[依赖与许可](DEPENDENCIES.md)），不构建
DOM。必须是一个完整对象，允许 JSON 首尾空白，拒绝 BOM、注释、尾随值、原始 NUL、
非法转义及未配对 UTF-16 surrogate 转义；合法 `\u0000` 和 surrogate pair 允许。
容器最多64层，根对象计一层，数组/对象都计层；解析中超限立即返回 `json_depth`。
库不能表示的数值（如 `1e999`）返回 `json`；准入不是任意精度数值服务。
重复键、字段名称、类型及业务范围尚未验证，调用方仍须按后续业务合同检查。
PING/PONG 必须恰为两个字节 `{}`；`{ }` 或 ` {} ` 对其他消息可合法，对心跳拒绝。

编码保留原Body的空白、中文和键顺序，不重新序列化；多字节字段按大端逐字节写入，
长度计原Body字节，不追加 NUL。输出分配前完整验证，不允许截断超限响应。
`decode_next` 仍允许头部合法但 Body 非法的原始拥有Frame；不改变M2.03合同。

## 统一响应及调用方策略

正常helper先完整验证请求，再显式映射PING→PONG、SERVER_INFO_REQUEST→
SERVER_INFO_RESPONSE、SEARCH_REQUEST→SEARCH_RESPONSE、SUGGEST_REQUEST→
SUGGEST_RESPONSE，并验证响应Body。它不执行查询，不决定无命中/无候选的业务字段。

错误helper只重新检查完整请求的头字段/实际长度，允许Body非法。六种状态分别产生：

| status | 固定Body |
|---:|---|
| 1 | `{"message":"invalid request"}` |
| 2 | `{"message":"overloaded"}` |
| 3 | `{"message":"stopping"}` |
| 4 | `{"message":"internal error"}` |
| 5 | `{"message":"deadline exceeded"}` |
| 6 | `{"message":"resource limit"}` |

`message` 仅诊断，客户端按头status判类；helper不复制任意异常/日志文本。错误Body也受
输出预算限制，容纳不了就返回 `body_length`，不递归造另一个错误，也不承诺交付。

调用方流程：`decode_next` 等待就保留输入；DecodeError关闭流，不猜ID或重同步；完整
Frame再验证Body。合法请求头下的非法Body可用INVALID_REQUEST，本地深度/计算预算可用
RESOURCE_LIMIT；非法响应由客户端关闭。正常业务完成才构造对应响应。EOF截断、输出
队列容量、断连和停止时的发送策略仍由后续连接模块接线。上述都是调用合同，当前没有
执行 socket 关闭/发送的代码。V2.0仍一连接最多一个在途业务请求。

## 验证入口与限制

`net.frame_codec` 在 Debug 和 Release 都执行显式行为检查：独立手工PING/PONG与
长度/ID字节、编码解码组合、Body上界/局部预算、字段/方向/status拒绝、UTF-8/JSON/
根类型/转义/心跳/嵌套、四种成功映射、六种错误状态、拥有性及定点分配失败保持。
不以同一生产encoder构造黄金期望，组合回环另作接线验证。

广泛损坏输入/fuzz批次、真实系统OOM、网络、并发、吞吐、跨平台和业务字段不在本次证据范围。
EOF、send/recv短写与连接生命未接线，内存成功不证明响应已到达对端。
