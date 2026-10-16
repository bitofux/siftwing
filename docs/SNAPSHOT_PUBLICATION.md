# 安全发布与旧查询生命期

`SnapshotManager::publish_candidate`显式接管完整只读Reader的共享所有权，
以C++17下的`shared_ptr`原子自由函数替换`current`。加载与发布分离：
[同步加载](SNAPSHOT_MANAGER.md)和[后台加载](SNAPSHOT_LOADER.md)成功都不会自动发布。

调用方是单控制者。空句柄或移空Reader返回`invalid_candidate`并保持current；
同一Reader对象返回`unchanged`，其他完整Reader返回`published`，即使版本相同也允许。
版本是完整uint64身份，0、最大值和回滚到较小值均合法，不用数值判断新旧。
候选须来自真实Reader成功加载，不能用可变别名移动/修改已共享Reader。
发布不分配、不读文件、不改变最后加载报告；拒绝与发布前异常不会切换在线版本。

每次`SearchService::execute`在worker开始计算时取得一次拥有句柄，响应版本、构建/来源、
搜索/推荐、文档、摘要/高亮和缓存都使用该句柄。已取得句柄的查询继续旧版本，
发布后才取得句柄的查询使用该次或更后的发布；排队但尚未execute的请求尚未绑定版本。
这不按TCP字节到达、任务准入、响应完成或对端接收的先后承诺版本。

```text
worker A取得100 ──保留100计算────────────────→ 返回100
控制者         ──加载101 → 显式发布101
worker B                              取得101 → 返回101
```

每个句柄/加载报告/调用方保留的回滚目标都是独立共享所有者。
最后一份所有权释放才回收Reader；管理器释放旧份额不等于旧Reader已销毁。
原子交换返回的旧份额在原子设施内部锁外释放，不在查询计算期间持发布锁。
不承诺原子共享指针无锁、硬时限、总内存/RSS上限或后台回收线程。

[缓存](VERSIONED_LRU.md)保留版本键，并在参数编码中加入完整规范manifest的SHA，
覆盖六载荷、统计、构建/来源及配置元数据。相同版本但不同成品不能命中另一份Body，
回滚到同一成品可复用原缓存；旧查询迟到写入仍归自己的内容身份。
不会通过清空缓存或无界版本历史保证一致性。摘要计算只读内存，仍有计算和分配成本，
SHA不是真实性认证。固定查询分词/停用资源、预算由实例隔离，调用方保证候选相容。

`SearchService`提供`try_load_candidate`、`take_loaded_candidate`、`publish_candidate`、
`current_snapshot`和`stop_loading`。加载器在第一次提交时按需创建，沿用单槽/异常交接合同。
控制入口串行；取成功结果后仍须明确发布。`stop_loading`永久关闭加载入口并排空join，
析构也先回收加载器再销毁其借用的Manager，不能与外部调用并发。

`OnlineServer`为创建线程提供对应的`try_load_snapshot`、`take_loaded_snapshot`、
`publish_snapshot`与`current_snapshot`入口。`run_once`不自动取加载结果或发布。
停止后提交返回`stopped`，发布返回`ECANCELED`；完成值仍可取。
停止先完成接入/提交静止，再回收Loader及查询worker，服务与信息来源覆盖最后调用。
网络关闭期限不取消已接收加载或查询，也不是整个退出的硬截止。

Sub可稳定借用只读`ServiceInfoSource`，每个SERVER_INFO响应从同一句柄生成版本/构建信息；
独立Sub的固定Body默认路径保持。业务完成/缓存/发布日志使用execute实际采用的版本，
Sub普通事件的版本只表示该日志时点观察，不冒充该请求的查询身份。
索引指标每次从同一manifest取版本及统计，跨组件指标仍非全局原子快照。

正式`siftwing-server`尚无reload命令或信号触发入口，默认不创建加载线程，
因此默认启动版本行为保持；这里提供并测试显式C++控制接口，signalfd接线属于后续工作。
真实CTest `snapshot.publication`验证独立八文件、原子读者、旧查询门禁、回滚、同版本异内容缓存、
真实TCP SEARCH/SUGGEST/SERVER_INFO、实际日志版本、停止排空与句柄寿命。
不以这些测试证明所有调度、实际系统OOM、性能或生产信号热更新。
