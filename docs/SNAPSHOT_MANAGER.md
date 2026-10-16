# 当前快照与同步加载结果

目标`siftwing_snapshot_manager`提供[SnapshotManager](../include/siftwing/snapshot/manager.hpp)，
拥有一个固定的当前快照，以及最后一次已完成加载的拥有报告。
`SearchService`使用管理器初始化，并在每次查询中保留一份`shared_ptr<const SnapshotReader>`；
响应头、版本缓存键和检索/推荐计算都使用这一个句柄。

`open(parent, version, limits)`复用[Reader完整校验](SNAPSHOT_READER.md)，
成功才返回非空管理器。初始`current()`和`last_load()->outcome`指向同一个完整Reader。
版本是显式uint64身份，包括0与UINT64_MAX；父目录、输入稳定性和26项预算沿用Reader合同。

`load_candidate(parent, version, limits)`在调用线程同步执行文件I/O、解析和全部校验。
成功报告拥有非空只读候选，失败报告拥有Reader原始诊断，两者都保留请求版本。
调用不会替换`current()`，也不要求候选版本递增。分配或未知异常直接上传；
完整报告创建前不替换`last_load()`，因此异常时当前快照及上一份报告保持。
初始加载异常同样不返回部分管理器。

`current()`不分配，可供多个线程并发调用，也可与候选加载并发：当前共享指针成员构造后
不再写入。`last_load()`和`load_candidate()`由控制者串行调用，不提供二者的并发读写保证。
管理器析构不能与成员调用并发；已经取得的句柄/报告可在管理器销毁后继续使用。
借用manifest、文档或索引时必须保留相应句柄，裸指针本身不延长生命期。

同时持有当前和候选会增加内存占用，外部持有的旧报告还会延长旧候选生命期；
Reader尺寸预算不等于进程RSS硬上限。最后一个共享所有者释放后Reader才销毁。
管理器自身没有后台线程、发布/回滚接口、信号入口或在线reload命令，当前服务版本仍固定。
[后台加载器](SNAPSHOT_LOADER.md)可独立构造，借用管理器并在专用线程调用同步候选接口；
从加载器构造至join返回，外部不得调用本管理器的last_load/load_candidate，current仍可并发读取。
安全发布需后续实现与验证。

真实CTest `snapshot.manager`使用独立手写八文件生成器，验证版本端点、空候选、
损坏SHA、重新签名后的非法语义、缺COMPLETE、原诊断、候选隔离、共享寿命、
多线程读取及管理器分配失败的状态保持。GNU ld包装只用于测试故障注入，
生产管理器不依赖包装或新增第三方库。该测试不证明任意调度、真实OOM或热切换能力。
现有在线搜索、缓存与指标集成回归检查生产接线后的固定版本行为。
