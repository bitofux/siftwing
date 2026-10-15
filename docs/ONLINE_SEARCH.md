# 在线搜索闭环

`SearchService`拥有完整只读Reader和固定分词/停用资源，`OnlineServer`组合Main、固定ProtocolSubReactor及WorkerPool。查询/推荐复用V1核心，worker不持Connection、socket或身份表。公共合同见[服务](../include/siftwing/query/search_service.hpp)与[控制者](../include/siftwing/net/online_server.hpp)。

```sh
build/release/siftwing-server --snapshot /absolute/snapshots --version 1 --port 9000
```

生产目标`siftwing_server`生成`siftwing-server`；只监听127.0.0.1。默认2个Sub、4个worker、128等待任务槽，`--subs`与`--workers`可设1..64，`--task-capacity`1..1024，启动后固定。端口0选内核端口，完整就绪后stdout报告`PORT N`。SIGINT/SIGTERM请求当前立即停止；`--run-ms N`可设有限运行时间（1..86400000），0为等信号。停止后输出接入/请求/响应/worker计数JSON；这些计数不证明对端收到。

启动先完整校验快照八文件/两支语义，显式装载资源，所有Sub及结果目标就绪后才开放监听。版本为规范uint64十进制（0和最大值合法），不解释CURRENT或自动找最新版本；启动失败不报告PORT。入口错误退出2、装载/启动/运行/停止失败1、stdout失败3、未知异常4、正常停止0。停止会丢弃连接输出，不能据此期待所有已接收请求都有响应。

## 业务Body

等待队列满时，Sub不等空位，向本次请求返回同ID OVERLOADED；错误输出服从连接硬限，容量释放后新请求可重新准入。合同、确定满队列及健康连接验证见[worker队列饱和](WORKER_SATURATION.md)。

沿[固定协议版本1](PROTOCOL.md)的24字节头和UTF8 JSON对象；业务字段严格检查，拒绝未知/重复键、错误类型与非整数k。请求完成后允许再次使用同一request_id；generation仍是连接身份，不是请求代次。每接纳任务至多发布一个结果。

| 请求 | 必需字段 | 可选字段和默认值 |
|---|---|---|
| SEARCH_REQUEST | `query`字符串 | `mode`为`or`或`and`（默认or）；`top_k`默认10；`summary_window`默认160；`summary_context`默认40 |
| SUGGEST_REQUEST | `key`字符串 | `top_k`默认10；`normalize_key`为bool（默认false） |

k为1..服务max_top_k；窗口1..4096，context为0..window-1。降低窗口须提供合法context。空/无词/全停用SEARCH及空/含空白SUGGEST键是INVALID_REQUEST；未知词/无匹配是成功空results。SUGGEST默认词键原样，显式normalize_key只沿已有规范化，不重新分词；使用者选词后另发SEARCH，服务不自动选第一项。

成功响应Body采用独立schema_version=1：SEARCH有command、mode、top_k、terms及results（原DocId、有限score、原标题/URL、摘要/HTML/spans）；SUGGEST有command、top_k、key、normalized和推荐项（term_id、term、distance、frequency）。共同字段含snapshot_version、build_version和对应支input_sha256/snapshot_config_sha256。SEARCH另有固定query_config资源SHA。身份为十进制字符串、UTF8/控制字节正确转义，网络Body没有CLI末尾LF；完整输出后才发布，不截断成成功结果。详细字段语义沿[CLI](CLI.md)，CLI原合同保持。

## 固定资源与预算

英文默认english，可用`--stopwords`。中文/混合搜索须显式`--tokenizer cppjieba --dictionary 文件 --model 文件`，可选`--user-dictionary 文件`与`--hmm on|off`。资源须与搜索建库规则相容；服务不从快照摘要恢复配置，也不把SHA声明当自动相容认证。停用、词典、模型逐组件拒链接/普通文件检查、有限装载，组合发布前重核SHA；加载完成后worker只读拥有内存，资源路径后续变化不改变当前服务。加载期间仍须文件稳定，不提供敌对写者的原子快照。

计算预算沿CLI profile1：查询原文/规范化各65536字节、k100、评分/完整结果显示/推荐距离各50000000工作，Reader单payload512MiB、单资源16MiB。网络成功Body上限为1MiB且再取task本地预算；固定安全ERROR仍受task预算。`--max-query-bytes`、`--max-top-k`、`--max-work`、`--max-output-bytes`、`--max-snapshot-bytes`、`--max-resource-bytes`只允许降低对应预算，正值含端点。请求不提供文件路径、资源或预算配置；预算不是RSS/CPU/总任务字节硬上限。

业务参数非法映射INVALID_REQUEST，工作/输出预算映射RESOURCE_LIMIT，非法显示元数据及未知内部失败映射INTERNAL_ERROR；ERROR使用原codec固定安全message，不泄露异常文本。任务未接纳full/closed沿既有OVERLOADED/STOPPING。任务接纳后生成结果、发布、应用、send排空及对端接收仍分别计量；无法生成错误结果、结果full/意外拒绝或通知失败记录首因并停止整体，不撤销accepted、不重投。此为安全兜底，生产饱和策略后续。

## 线程与停止

每连接从完整请求交付到响应排空保持[单在途门禁](SINGLE_INFLIGHT.md)，包括PING、错误和
拒绝响应；等待worker或部分输出时不交付下一帧。SHUT_WR保留完整请求，排空后主动继续
缓存帧，不要求客户端另发字节；排空不表示对端收到。不同连接仍可独立推进。

OnlineServer的create/run_once/port/report/stop/析构由创建线程串行；Main在该线程只接入。各Sub提交拥有请求，worker共享稳定只读服务，生成最多一个RoutedCompletion并[发布通知](COMPLETION_WAKEUP.md)，所属Sub校验完整连接身份/等待请求后复制输出并预算发送。服务及提交器生命覆盖全部worker/Sub join。

停止先Main close/销毁返回，请求所有Sub停止并等quiescing（最后提交器调用已返回），随后WorkerPool close/drain/join，完整发布及通知都返回后撤目标、release各Sub，再join其连接/结果/fd清理。quiescing报告不证明queue已经closed，closed/empty/worker统计不证明调用静止。部分启动失败按同顺序回滚；通知fd不能在晚到write前关闭。

真实CTest覆盖生产indexer→server→TCP查询/推荐、独立数学/Unicode参考、英文稀疏身份/中文共享只读资源、参数/预算/启动失败，以及部分启动分配故障、worker分配异常固定安全响应、真实结果full、accepted后通知失败和晚到通知停止屏障。插桩范围以实际日志为准；不证明任意调度、DSO内部、真实OOM、质量或性能。

SERVER_INFO仍保持未接入时的INVALID_REQUEST；正式客户端模式、请求deadline/取消、全局过载策略、版本缓存、热切换、signalfd和优雅排空属于后续模块。

每连接已有[输出水位与硬限策略](OUTPUT_BACKPRESSURE.md)，可由OnlineServerConfig.sub.output配置，
命令当前使用自动默认值。输出过大时Sub尝试同ID固定RESOURCE_LIMIT，错误也无法容纳则局部关闭；
worker和结果邮箱的拥有、发布与停止屏障保持。低位仍不允许在响应排空前接纳下一请求。
