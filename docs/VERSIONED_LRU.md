# 版本感知进程内LRU

`SearchServiceConfig::cache`提供可选实例缓存，默认`capacity=0`关闭；可由`--cache-capacity`、`--cache-bytes`、`--cache-max-value-bytes`或[启动配置](SERVER_CONFIG.md)显式启用。
服务持有固定快照和分析资源，缓存只拥有成功的JSON Body，包含成功空结果，不保存错误、任务、
连接、fd或完整响应帧。每次命中仍经worker构造当前request_id的Completion并回投所属Sub；
既有超时、单在途、输出预算及停止屏障继续适用。索引热切换尚未实现。

```cpp
siftwing::query::SearchServiceConfig config;
config.parent = "snapshots";
config.version = 0;
config.cache = {128, 8U * 1024U * 1024U, 256U * 1024U};
auto service = siftwing::query::SearchService::create(config);
```

## 键与一致性

精确元组包含完整uint64索引版本、SEARCH/SUGGEST类型、有效查询文本和参数编码。
SEARCH使用既有UTF-8规范化规则；SUGGEST只在normalize_key=true时规范化，标志也进入键。
参数用JSON数组无歧义编码，SEARCH含top_k、AND/OR、摘要窗口、上下文及有效Body预算，
SUGGEST含top_k、normalize_key及有效Body预算。解析后省略默认值与显式默认值等价。
不同字段不作字符串分隔符拼接；request_id和连接身份不进入键。

严格业务schema和原始文本预算先于lookup；暖缓存不能绕过重复键、未知字段、非法参数或
原查询字节限制。有效输出预算取服务预算与task预算较小者，暖大响应不会用于小预算任务。
固定分词/停用词、工作预算和资源策略由服务实例隔离，多个服务不共享缓存。

## 容量、线程和失败

独立`VersionedLru`接口见[公共头](../include/siftwing/cache/versioned_lru.hpp)。容量为
0..4096条，驻留charge预算为正且最多64MiB，单Body预算为正且最多1MiB、不得超过驻留预算。
等于上限可存。charge是两份键字符串长度加Body长度，包含map/list的键副本；不计容器元数据、
string容量、分配器、返回副本或在途结果，因此不是进程RSS硬限。

map精确查找O(log capacity)，list维护最近使用；get和成功put都提升MRU。更新同键不增加条目；
过大put返回false并保持旧同键。完成全部潜在分配后才提交/真实淘汰，分配失败保持旧内容、顺序与
统计。get返回独立拥有副本，淘汰或缓存销毁后仍有效，空Body与miss分开。

单mutex保护容器、复制与统计；业务规范化/查询在锁外，同键同时miss允许重复计算，无single-flight。
缓存get副本或put分配失败时，服务尝试原计算或跳过缓存；其他初始化/同步/未知异常沿原服务与
worker合同。构造/析构须无并行调用，服务生命继续覆盖全部worker join。

`SearchService::cache_stats()`可并发返回同一锁内的hits、misses、evictions、当前entries/bytes；
累计计数饱和，0容量独立LRU的get计miss，但关闭缓存的SearchService直接绕过lookup，统计保持0。
`OnlineServer`拥有服务；控制者可在交接所有权前保留稳定借用并在服务生命内读统计，不跨stop销毁。

## 验证与边界

CTest注册`cache.versioned_lru_test`、`cache.versioned_lru_concurrency_test`、
`cache.versioned_lru_failure_test`及`cache.service_1/2`。
独立vector参考覆盖键、最近使用、双预算和统计；普通new逐分配失败验证事务；多线程混合版本/类型
检查命中不串值。真实indexer用公开原创输入建立0与UINT64_MAX两版本快照，覆盖参数/预算隔离、
空结果、错误不存、超大成功旁路、共享服务并发及1/2个Sub的TCP回包、半关闭、续读和关联号。

小型计时仅输出当前小语料/当前进程的关闭缓存与暖缓存耗时，无速度门槛，不外推生产吞吐或最坏延迟。
未实现TTL、Redis、热切换、分片、取消或优雅退出，不提供缓存命中绕过worker队列的旁路。
