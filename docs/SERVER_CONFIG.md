# 服务启动配置

`siftwing-server --config 文件.json` 在启动线程中一次读取严格 JSON 配置，按
**默认值 → 配置文件 → 显式命令行** 合并。原有纯命令行用法继续可用。
例如已用 indexer 在 `./snapshots/1` 生成完整快照后，从仓库根目录执行：

```sh
build/release/siftwing-server --config docs/server.example.json --port 9000
```

示例文件保留 loopback，路径与版本需要对应自己的建库输出。`--help` 列出参数。
所有命令行选项成对，重复选项拒绝；`--help` 仅单独使用。配置文件位置不影响资源路径：
配置、快照和资源的相对路径均相对于**启动工作目录**，不是配置文件目录。
没有环境变量插值、隐式 CURRENT、自动查找文件或热更新。

## 文件与类型

根必须是平面对象，`schema_version` 必须为整数 `1`。未知、重复键（包括转义后同名）、
子对象、数组、注释、尾逗号、非法 UTF-8、类型错误、超范围值均拒绝。
每个文件字段先独立检查，不能用 CLI 覆盖掩盖非法值；必需字段与跨字段关系在合并后检查。
文件和 CLI 都不能提供空字符串或含 NUL 的值。

`snapshot` 和 `version` 必须在文件或 CLI 显式提供；版本是规范 uint64 十进制**字符串**，
例如 `"0"`、`"18446744073709551615"`，拒绝 `"01"`、正负号。其他数字使用非负 JSON 整数，
不接受字符串、浮点数或布尔值。开关使用 JSON 布尔值；对应 CLI 使用 `on|off`。
CLI 数字也必须为规范十进制。下表文件键的下划线替换为连字符，再加 `--` 就是 CLI 选项，
如 `cache_capacity` 对应 `--cache-capacity`。`schema_version` 仅在文件中使用。

| 文件键 | 默认值 | 范围或含义 |
|---|---|---|
| snapshot / version | 必填 | 快照父目录 / uint64 十进制字符串 |
| listen_address | `127.0.0.1` | 数值 IPv4/IPv6；无 DNS、zone 或隐式双栈；IPv6 为 V6ONLY |
| port / backlog | 0 / 128 | 0..65535（0为内核选端口）/ 1..2147483647，实际 backlog 受内核约束 |
| reuse_address / tcp_no_delay | false / false | SO_REUSEADDR / 新连接 TCP_NODELAY |
| subs / workers | 2 / 4 | 各 1..64，启动后固定 |
| task_capacity | 128 | 1..1024，共享等待任务槽，不含正在执行任务 |
| handoff_capacity / completion_capacity / max_connections | 各128 | 各 1..1024，**每个 Sub** 的条目/连接槽 |
| handoff_budget / completion_budget / io_attempt_budget / accept_attempt_budget | 各16 | 各 1..1024，有限轮次预算 |
| max_body_bytes | 1048576 | 2..1048576，每连接 Body 上限 |
| read_byte_budget / write_byte_budget | 各65536 | 各 1..1048600，每连接每轮字节预算 |
| output_low_bytes / output_high_bytes / output_hard_bytes | 各0 | 全0自动解析；否则 `0<=low<high<=hard<=24+max_body_bytes` 且 `hard>=26` |
| idle_timeout_ms / request_timeout_ms | 各0 | 各 0..86400000；0关闭，语义见[期限](DEADLINES.md) |
| run_ms / shutdown_ms | 0 / 1000 | 各 0..86400000；运行时间0等待信号；关闭0立即截止网络排空 |
| tokenizer | `english` | `english` 或 `cppjieba` |
| stopwords / dictionary / model / user_dictionary | 未提供 | 资源路径；cppjieba 必须有 dictionary、model，可选 user_dictionary 与 hmm；english 拒绝中文资源和显式 hmm |
| hmm | true | cppjieba 的固定 HMM 选择 |
| max_query_bytes / max_top_k / max_work | 65536 / 100 / 50000000 | 正整数，仅可降低默认上限 |
| max_output_bytes / max_snapshot_bytes / max_resource_bytes | 1048576 / 536870912 / 16777216 | 正整数，仅可降低默认上限 |
| cache_capacity | 0 | 0..4096，0关闭，缓存成功 Body |
| cache_bytes / cache_max_value_bytes | 8388608 / 262144 | 1..67108864 / 1..1048576；单值预算不得超过总预算，即使关闭缓存也验证 |

查询预算不足时返回既有业务错误，不会自动缩小请求的默认 top_k；配置条目和字节预算不等于
进程 RSS 或 CPU 的硬上限。线程数与每 Sub 连接数的乘积也不证明实际并发承载能力。
`shutdown_ms` 只限网络排空，仍安全等待 worker join，不提供进程总退出硬上限。

配置文件必须为非空、至多 64KiB 的普通文件；逐路径组件拒绝符号链接与 `..`，不跟随父目录链接。
读取期间观察到大小/时间变化会拒绝；固定 fd 和前后 stat 不构成敌对并发写者的原子快照。
启动期间文件仍应保持稳定。相邻资源文件沿各自装载器的检查，不在配置解析阶段读取。

## 启动与错误

```text
argv → 读取/解析配置 → 字段准入 → CLI覆盖 → 组合检查
     → 装载完整只读快照/资源 → 创建worker/Sub/Main → PORT N
```

普通参数/文件/JSON/数值/必需项错误分别报告 `config.argument`、`config.file`、`config.json`、
`config.value`、`config.required` 上下文，退出2且不发布端口。未知文件键沿 `config.argument`。
快照、资源或服务启动失败退出1；stdout失败3；分配或未知库异常4；帮助正常输出0。
配置验证不修改任何输入，失败 RAII 回收本次 fd/临时对象，不启动部分服务。

`ServerSettings` 拥有路径与监听字符串，可复制/移动；`online_config(settings)` 返回的监听地址
借用源 `settings.listen_address`，应在创建服务前保持稳定，示例入口持有至服务销毁。
worker 不读取配置文件。启动后修改文件不改变已运行配置，重载需未来独立设计。

## 验证入口

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^config\.'
```

原生测试覆盖默认/覆盖/地址拥有、字段与组合边界、严格 JSON、64KiB、符号链接、FIFO 和失败 fd 回收。
进程测试使用原创快照及独立数学/线格式参考，验证 cwd 路径、文件删除后服务、四命令、空闲关闭和
显式 IPv4/IPv6；未测任意网络环境、十万连接、热更新、敌对写者或硬实时期限。
