# 网络客户端

`siftwing-cli --network`通过TCP访问[正式在线服务](ONLINE_SEARCH.md)，提供PING、SERVER_INFO、SEARCH、SUGGEST。原[本地快照模式](CLI.md)继续使用原参数与输出；网络模式不读取本地快照、词典或停用文件。

```sh
build/release/siftwing-cli --network PING --port 9000
build/release/siftwing-cli --network SERVER_INFO --port 9000
build/release/siftwing-cli --network SEARCH --port 9000 --query 'red blue' --mode and --top-k 5
build/release/siftwing-cli --network SUGGEST --port 9000 --key CAT --normalize-key
```

通用参数：`--host`默认127.0.0.1，仅数字IPv4/IPv6；`--port`必需，1..65535；`--timeout-ms`默认5000、1..86400000；`--request-id`默认1、非零uint64；`--max-response-bytes`默认1048576、2..1048576，按响应Body字节计。数字必须为规范十进制，拒绝前导零、正负号、重复与未知选项。

SEARCH必需`--query`，可选`--mode or|and`（默认or）、`--top-k`（默认10，1..100）、`--summary-window`（默认160，1..4096）与`--summary-context`（默认40，0..window-1）。降低窗口时也须设置合法context。SUGGEST必需`--key`，可选同范围`--top-k`及布尔开关`--normalize-key`。PING/SERVER_INFO不接受业务选项；业务空词/非法词键由远端按其资源和schema判断。

成功时原样输出已验证响应Body，再加一个LF。SEARCH/SUGGEST字段见[在线业务schema](ONLINE_SEARCH.md)，客户端校验的是协议JSON对象，不另行校验业务结果schema。失败时stderr诊断；传输或协议失败退出1、参数错误2、stdout错误3、未知异常4；有效ERROR_RESPONSE退出5，保留远端status，不输出成功JSON。stdout自身失败可能留下前缀，调用者须检查退出码。

## 交换、期限与资源

公开API见[client.hpp](../include/siftwing/net/client.hpp)，CMake目标`siftwing_client`。每次exchange独占一个nonblocking/CLOEXEC socket，返回拥有Frame或拥有诊断；请求和配置只在调用期间借用，源不改变。分配与未知异常传播，RAII仍关闭socket。独立调用可并发，没有持久连接或自动重试。

[协议版本1](PROTOCOL.md)使用24字节大端头。send处理短写，recv增量拼帧；EINTR重试、EAGAIN由poll等待，connect就绪后再读SO_ERROR。连接、发送与接收共用一个steady_clock总期限，少量进展不续期；请求编码在期限前，不保证调度硬实时。期限到达关闭客户端连接，不取消远端worker。

响应须完整通过方向、消息类型、当前request_id、长度预算、UTF8与JSON对象准入；PONG必须恰为`{}`。半帧EOF、RESET、损坏头/Body或错误关联号均失败。拒绝同次recv已观察到的首帧后余量；不等待EOF来证明未来没有额外字节。没有DNS、TLS、重连、管线或多在途请求。

## SERVER_INFO

正式服务仅接纳空JSON对象（允许JSON空白），所属Sub按次取得完整拥有信息，单响应内版本/构建字段一致，继续服从单在途、输出硬限和请求期限。非空对象为INVALID_REQUEST；响应超Body预算或输出硬限沿RESOURCE_LIMIT替代／局部关闭。默认未配置元信息的独立Sub和旧协议探针仍拒绝该请求。

成功Body字段：

| 字段 | 含义 |
|---|---|
| `schema_version`、`command` | `1`、`SERVER_INFO` |
| `protocol_version` | `1` |
| `snapshot_version` | 完整快照uint64的十进制字符串 |
| `build_version` | 快照声明的构建身份，不是运行程序的Git身份 |
| `commands` | PING、SERVER_INFO、SEARCH、SUGGEST |
| `limits` | 服务查询Body输出上限`max_body_bytes`、查询字节`max_query_bytes`、`max_top_k` |
| `cache_enabled` | 固定服务配置是否启用LRU |

不包含路径、凭据、缓存命中统计或动态负载。`limits`描述SearchService预算，连接还可能有更低的Sub Body/输出预算，不把这些字段当作可接纳任意请求的保证。OnlineServer启动时生成并覆盖各Sub的server_info_body；SearchTask/Completion仍只支持SEARCH/SUGGEST，无载荷迁移。

## 验证入口

CTest的`net.client_integration`使用原创快照与独立Python数学/Unicode参考，验证真实四命令；`net.client_protocol`用独立线字节验证分片、损坏、错误状态、总期限、RESET、输出失败及可用IPv6；`net.client_arguments`验证严格参数；`net.client_native`通过真实loopback与GNU静态包装验证短I/O、EINTR、故障/异常与fd归还。原本地CLI、旧探针及正式服务测试分别保留各自合同。有限插桩与loopback通过不证明公网、真实OOM、任意调度或生产性能。
