# V3 故障恢复与有界持续运行验证

服务已具备完整快照加载、显式发布和 Main signalfd 终止控制。这里验证这些能力持续交错时的
结果一致性和资源回收，生产接口与默认行为沿用原合同。

## 自动测试

完成常规 CMake 配置和构建后运行：

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^v3\.'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^v3\.'
```

| CTest | 输入与时序 | 检查的结果 |
|---|---|---|
| `v3.fault_recovery` | 8轮缺完成标记、同长度载荷SHA损坏、重新签署SHA但语义非法的词表；每轮真实fd耗尽后恢复 | 24次坏加载保留原current；8次`EMFILE`保留Reader诊断；释放资源后可查询、加载、发布和回滚，fd回落 |
| `v3.soak_1`、`v3.soak_2` | 1/2个Sub、2个worker、等待容量8、LRU容量16；两客户端持续搜索/推荐，同时控制者坏加载、发布、回滚和同数字版本异内容切换 | 黄金帧、请求ID、状态、版本/build/文档身份或推荐词一致；每32轮检查fd/线程不增长；结束后验证已接受加载报告、worker计数与资源回收 |
| `v3.shutdown_races` | 8轮×4组合：合法/失败候选与正常排空/零网络期限，交替INT/TERM | 每次确定有执行中查询、排队查询及暂停Loader；信号不提前关闭移交目标，查询执行一次，Loader排空join，Sub静止，失败候选不发布，全部fd回收 |

样本由 Python 独立编码，未使用生产 Writer 生成正确答案，不需要私有语料。
词表语义反例具有正确外层SHA；TCP响应由独立24字节参考核对。
资源基线在一次空线程创建/join且该线程的`/proc`条目消失后采集，以纳入TSan首次建线程时
惰性初始化的运行时后台线程，并避免把刚join线程的短暂条目算进基线；
收口仍严格比较线程数，不过滤线程或放宽上限。
每个测试子进程只向自身发送信号；fd压力仅降低本进程的`RLIMIT_NOFILE`软限至最多64，
以`/dev/null`占满后确认`EMFILE`，再关闭填充fd并恢复原限。不会降低硬限或修改系统配置。

`v3.shutdown_races`复用快照发布集成目标中的静态链接包装，仅用于确定执行与join顺序，
仍调用真实Reader、查询和关闭实现；生产程序没有故障环境变量或测试hook。
其他三项直接使用生产接口。所有断言在Release下仍执行。

## 延长持续运行

默认CTest持续运行至少2秒、至少16轮；这保证日常回归有限且有真实交错。
可独立延长为120秒或更长，最大3600秒：

```sh
python3 -B tests/integration/v3_fault_soak_test.py \
  --native build/release/siftwing_v3_fault_soak_test \
  --parent build/release --suite soak --subs 2 --seconds 120
```

原生程序输出JSON：实际轮数、成功响应数、包含收口的实际毫秒数、稳定阶段fd/线程数、
进程峰值RSS及最终fd/线程数。RSS是Linux`getrusage`的峰值观测，没有用固定RSS阈值判断泄漏。
快照旧句柄以固定64槽`weak_ptr`采样，在服务销毁后检查过期，不保存无界历史。

驱动成功后删除本次原创临时输入，命令执行者须保存stdout及源码/二进制身份以复现结果；
失败、超时、非空stderr或输入变化时保留并报告临时目录，含输入、哈希基线和已有原始输出。
外层CTest与驱动均有超时，延长运行时驱动超时为指定秒数加100秒。

## 证据边界

以上测试与[既有V2故障矩阵](V2_TESTING.md)、[加载与安全发布](SNAPSHOT_PUBLICATION.md)、
[终止信号](SIGNALS.md)和[优雅关闭](GRACEFUL_SHUTDOWN.md)互补。
已有V2测试覆盖队列满、慢输出、断连和fd复用；这里聚焦V3能力组合，不更改生产错误策略。

有限时长、有限输入及受控调度不能证明所有竞态或任意长时间稳定；测试报告须记录实际
命令、时长、配置、退出码、源码/二进制身份和失败原件。真实fd耗尽不等于全系统内存耗尽；
未进行真实OOM、磁盘故障、机器崩溃或生产负载验证。峰值RSS、完成轮数和响应数用于核实
测试确实推进，不作为吞吐、延迟或容量基准；性能测量另行进行。

网络排空期限仍不取消加载/worker，也不是总退出硬截止。测试通过不代表学习者已经掌握。
