# 后台候选加载

`siftwing_snapshot_loader`提供[SnapshotLoader](../include/siftwing/snapshot/loader.hpp)，
显式构造时启动一个专用线程，串行调用[SnapshotManager](SNAPSHOT_MANAGER.md)的
`load_candidate`。文件I/O、解析、八文件完整性、跨文件语义及预算验证均由真实
[SnapshotReader](SNAPSHOT_READER.md)完成。加载自身不改变当前快照，成功不等于[显式发布](SNAPSHOT_PUBLICATION.md)。

管理器由调用方拥有，须稳定存活且晚于加载器销毁。构造加载器之前结束已有
`last_load`/`load_candidate`调用；从构造到`join`返回，这两个入口由加载器独占，
不能由外部访问或交给第二个加载器。`current`可继续并发读取，单控制者可显式调用`publish_candidate`。
`join`后调用方恢复管理器原有的串行控制，已取得的报告和快照句柄独立延长Reader寿命。

`try_submit(SnapshotLoadRequest)`按值取得父目录路径、精确强类型版本和独立预算。
路径字符串归请求拥有；输入目录和文件须在实际异步加载期间保持可信稳定，
不能只保证提交瞬间不变。版本0、UINT64_MAX、相同或回退均合法，不自动选择磁盘最新版本。
非法路径、预算或快照由后台Reader给出普通诊断。

唯一任务槽覆盖等待、执行和完成未取走三种阶段。返回`accepted`表示接受并执行一次；
`busy`表示槽仍占用，`stopped`优先于`busy`。拒绝不排队、不覆盖已有任务或报告。
方法可以等待短mutex，不等空位或加载结束；不承诺无锁、硬时限或公平性。
参数构造和加锁异常在接受之前上传。锁不包I/O、解析或管理器旧候选的释放。

`take_completed()`不等结果：未完成返回空`optional`，完成时按值取走并释放槽。
完成值保留`requested_version`，恰有以下一个字段非空：

- `report`：原管理器拥有报告，其中`outcome`是完整非空候选或Reader原诊断。
- `exception`：原加载异常的`exception_ptr`，调用方可重抛，包括分配及非标准异常；
  不伪装成普通Reader错误，管理器原`last_load`与当前快照保持。

正常成功和普通失败沿管理器合同替换最后报告；异常保持旧报告。
后台捕获加载异常后继续等待下一任务。状态同步设施失败属于不可恢复错误，
worker边界及析构回收失败会`terminate`，不冒充候选加载失败。

`request_stop()`幂等关闭提交入口并唤醒空闲线程，不取消已接收任务。
`join()`先停止再排空，返回后任务已完成，尚未取走的结果仍可取；重复调用有效。
只由一个非加载线程控制者串行`join`，self-join拒绝且不关闭入口。
提交、取结果和停止可以并发，但对象须稳定存活；销毁前撤销并等待全部外部调用。
析构自动停止、排空并join。没有I/O取消或关闭硬截止，文件I/O阻塞可能延长回收。

一个槽限制尝试数量，不是内存硬上限。当前、候选和外部保留的旧报告仍可同时占内存；
Reader尺寸预算不等于RSS。专用线程避免占用查询WorkerPool，但仍竞争CPU、内存和磁盘带宽。

`SearchService`及`OnlineServer`已提供按需创建加载器的显式控制接口，加载成功后仍须单独发布。
正式CLI尚无reload/信号触发入口，默认不创建加载线程，启动版本行为保持。

真实CTest `snapshot.loader`用独立Python八文件生成器与真实后台Reader，
验证完整校验、旧快照并发读取、单槽拒绝、输入拥有、异常交接/恢复、停止排空、析构和共享寿命。
GNU ld包装只用于确定门禁与分配/加载异常注入；不使用私有标准库类型拦截线程启动，
不证明实际系统OOM、线程创建系统错误、线程资源耗尽、任意调度或生产热更新能力。
