# 结构化日志

`siftwing-server`默认`--log-level info`，自动记录生命周期、连接与异常；逐请求细节须显式debug。
使用spdlog 1.15.3同步输出JSON Lines，
不创建后台线程或全局注册实例。stdout仍只输出`PORT N`和停止报告，日志走独占复制的stderr，
或由`--log-file`指定的新文件：

```sh
mkdir -m 700 ./logs
build/release/siftwing-server --snapshot ./snapshots --version 1 \
  --log-level debug --log-file ./logs/run-001.jsonl
```

配置文件的对应平面字段为`"log_level":"debug"`与`"log_file":"run-001.jsonl"`。
字段类型、合并次序沿[启动配置](SERVER_CONFIG.md)。级别仅接受`off/debug/info/warn/error`；
阈值包含更严重事件。可显式off供测试或性能对照；off与log_file组合拒绝。
只提供log_file时沿默认info启用；不设置文件时目标为stderr。

## 输出目标与失败

相对文件路径按启动cwd解释，不创建父目录；逐组件拒绝`..`和符号链接，以`O_EXCL`新建0600普通文件，
启动时拒绝已有当前文件和配置范围内的归档；文件输出自动按大小轮转。每次启动使用全新文件族名称。
最终父目录须由当前有效用户拥有，且不允许组/其他用户写入；本文件族禁止其他进程/实例或外部工具并发改写。初始化失败在装载服务/创建线程前退出1；
参数或组合错误退出2。初始化分配/未知异常沿入口退出4。创建后的空文件即使后续初始化失败也保留。
尚未建立logger时诊断沿原有stderr文字；建立且启用后，运行失败使用固定结构，不混入异常文本。

实例拥有CLOEXEC资源。stderr在初始化后被重定向，不会改变已复制的输出目标。
文件路径先逐组件检查并固定父目录fd；spdlog通过/proc/self/fd使用该目录，文件open回调先O_EXCL预留0600文件，
随后核对真实文件身份、CLOEXEC并禁用stdio缓冲。不改变进程umask；不防同一用户恶意并发替换文件族。
正常行不超过2048字节，同实例并发写入不交错；不协调其他实例或其他进程写同一个stderr。
同一实例的sink内部使用互斥锁，业务调用方无需再加日志锁；序列化使用调用局部值，在sink锁外完成。
日志是同步I/O，可能等待锁、设备或管道，**没有硬时间界、fsync或断电持久性保证**。
不在性能数据中假定日志开销为零，不由行上限推导整个进程内存上限。

序列化、分配或输出失败由日志层捕获，`Logger::failed()`单向置true，后续输出停用，业务继续。
最后一行可能只有前缀；不重试整行、不递归输出错误。stderr fd适配的短写继续剩余字节，EINTR重试最多128次；
原生轮转文件使用spdlog的stdio写入路径，写入错误同样停用实例，不套用stderr的重试次数合同。
闭管道通过当前线程临时屏蔽SIGPIPE并消费本次EPIPE产生的新信号隔离，恢复原mask，不改进程handler。
调用前已有pending信号保留。日志失败标志不是成功送达证明；如果输出本身坏了，就无法依赖该目标报告故障。

## 文件大小轮转

非空`--log-file`自动使用spdlog 1.15.3原生`rotating_file_sink`，同步、无后台线程；
外层sink mutex负责并发和sticky失败，原生单线程sink只在该锁内调用，业务无需第二把日志锁。
不指定文件仍默认写stderr，stderr不由程序轮转；如需journald收集/保留须由部署方另行配置，本仓库未部署验证。

```sh
mkdir -m 700 ./logs
build/release/siftwing-server --snapshot ./snapshots --version 1 \
  --log-file ./logs/run-001.jsonl --log-max-bytes 10485760 --log-max-files 3
```

JSON字段为`log_max_bytes`、`log_max_files`，默认10MiB/3份归档；范围分别为2048..1073741824、1..20。
参数启动读取，不热更新；stderr不使用这些参数，但仍执行范围校验。当前文件及最多3份归档命名如下：

```text
run-001.jsonl       当前文件
run-001.1.jsonl     最近归档
run-001.2.jsonl
run-001.3.jsonl     最旧归档；下一轮由spdlog淘汰
```

完整日志行将使当前文件超过上限时，先轮转再写该行，不拆行。项目单行<=2048字节且文件上限>=2048，
正常独占运行下每文件不超过配置值，整个文件族内容字节上界为`(log_max_files+1)*log_max_bytes`；
该数值不含文件系统元数据、块分配及程序外持有已淘汰文件fd的空间，也不代表整个进程的内存或磁盘上界。
只在有下一行写入时触发，不做时间轮转、压缩、启动追加或跨进程共享。

启动拒绝已有当前文件或`.1`至配置份数的归档（含链接/FIFO/目录），不认领旧日志；每次重启使用新文件族名，
不同运行的历史文件族需要使用者另行归档/清理，本策略只限制单次运行的文件族，不限制跨重启累计占用。
归档移动/淘汰由spdlog原生实现；文件open回调会阻止重命名失败后的回退截断旧当前文件。
任一步失败可能已经移动部分归档，不回滚、不能保证最后一行写入；置failed后同实例后续记录停用，业务继续。
文件无stdio待刷缓冲，正常写入返回不等于fsync或断电持久化。

不要再对同一文件族运行外部logrotate或其他写入者：外部rename不会触发本服务自动reopen，
copytruncate存在竞争/丢失窗口；同一有效用户恶意修改文件族不属于此准入/轮转合同的保证范围。
异步输出与轮转独立，本模块仍是同步日志。

## schema_version=1

普通事件每行固定包含以下17键；指标事件另加[最小指标](METRICS.md)定义的4键。没有任意message、查询、正文、凭据、路径或异常what()字段。

| 字段 | 类型与含义 |
|---|---|
| schema_version | 数字1 |
| time_unix_us | 十进制字符串，系统时钟Unix微秒；可回拨，不用于计算耗时 |
| level / module / event / code | 固定名称；稳定code分类与数值native_error共同解释失败 |
| thread_id | 十进制字符串，实际记录线程的Linux TID |
| reactor_id / connection_id | 数字或null；connection_id是Sub内slot，须结合reactor和generation |
| generation / request_id / index_version | 十进制字符串或null；保留完整uint64，0不是缺省 |
| elapsed_us | 非负整数或null；worker服务execute的steady_clock耗时，含缓存/查询计算，不含等待队列与网络 |
| result_count | 非负整数或null；成功Body中results数组长度，不记录数组内容 |
| cache_state | null或disabled/miss/hit/unavailable；null表示尚未到lookup，不是miss |
| native_error | 数字，正常0；io类别为errno，business_error为线上ErrorStatus |

日志类型可以表示0；真实线上请求号仍须满足[协议](PROTOCOL.md)的非零约束，generation沿连接身份合同非零。
完整关联使用`(reactor_id, connection_id, generation, request_id)`，request_id允许完成后复用，
不是全局唯一追踪号；连接代次也不是每请求代次。无连接的启动日志上下文为null，装载后生命周期携带真实版本。
日志仅标记本机观察阶段，不提供全局序号，跨线程文本顺序不构成因果顺序保证。

## 事件与级别

| 级别 | 事件 |
|---|---|
| info | server_starting/started/stopped，connection_opened及正常connection_closed，metrics_snapshot |
| debug | request_received，task_submitted，cache_lookup，成功query_finished，completion_published/applied |
| warn | 异常connection_closed（protocol_error/timeout/io_error），task_rejected，失败query_finished，completion_rejected/stale，output_limited，idle_expired/request_expired |
| error | server_failed |

队列提交成功可能在worker输出之后才打印，因为真正交接发生在打印前；不因此判定乱序交接。
缓存状态来自每调用独占观察值，不用多线程共享计数器差猜命中。查询指标始终计时；仅debug开启且实例健康时导出逐请求耗时、
缓存观察和成功结果数量；结果Body解析受1MiB/深度64限制，异常只停用日志，不改变查询结果。

`query_finished`表示服务execute已返回；`completion_published`表示结果入队及通知成功，
`completion_applied`表示Sub复制响应或硬限替代错误；这些都不证明输出排空或对端收到。
业务拒绝、超时、硬限与关闭保留各自含义，不把关闭日志一律解释为服务故障。

## 验证入口

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^logging\.'
```

原生测试覆盖并发、过滤、完整uint64/null、隐私、文件准入；stderr fd适配覆盖短写/EINTR、部分写停用、ENOSPC及真实闭管道。
原生轮转覆盖大小/保留份数/最新连续后缀、并发整行无重复、0600、归档冲突及重命名失败不截断；真实进程验证CLI参数与轮转期间响应。
真实服务测试验证未指定级别时stderr/新文件默认info、debug过滤、warn保留及显式off；
独立解析JSON并用独立排名/推荐参考验证四命令、同连接miss/hit、错误后继续、线程与关联、
空闲关闭及坏日志目标下业务继续。Release保留行为检查；并发专项可在独立TSan构建中运行logging.contract，
ASan/UBSan使用另一构建。测试不证明日志无阻塞、持久化、高并发容量或生产性能。

## 网络模块名称迁移

当前连接日志module为`reactor`，不再为`sub`。消费日志枚举的脚本应同步迁移；事件、身份、安全过滤及行预算保持。旧日志原件保留当时的sub标签，不回写历史。
