# 有限eventfd通知原语

公共合同见[eventfd_wakeup.hpp](../include/siftwing/net/eventfd_wakeup.hpp)，结果发布组合见
[Completion通知](COMPLETION_WAKEUP.md)。不再提供fd交接队列或try_handoff_and_notify兼容接口。

create_eventfd_wakeup单次创建普通聚合eventfd，NONBLOCK/CLOEXEC、不用SEMAPHORE；成功返回UniqueFd
（含fd0），失败保存原generic errno，创建EINTR不重试。通知/清空无分配、无关闭、无异常，不保持errno。

| 接口 | 保留合同 |
|---|---|
| notify_eventfd(fd) | 写8字节的1，EINTR最多64次尝试；饱和EAGAIN表示已有合并通知；短写EIO，负fd EBADF |
| drain_eventfd(fd,budget) | 成功read和EINTR均占预算；EAGAIN为drained、耗尽为budget_exhausted、其他失败保诊断；记录成功读取次数但忽略计数值；零预算EINVAL |

fd须是覆盖整个调用的稳定非阻塞普通eventfd借用，不能把EAGAIN语义推广到文件/管道。
消费者有限清通知后检查真实队列，队列非空或预算耗尽时主动续轮，不能按计数pop或立即无限等待。
队列锁建立数据可见性，eventfd不是数据存储/内存同步的替代；通知成功不证明消费者已运行。

生产只有一个结果eventfd。只有Completion入队accepted才通知；通知失败不撤销已提交结果，也不允许重投。
关闭须停止准入并完整等待所有发布/通知返回，再撤目标/注销/关闭；closed/empty不证明发布者静止，
过早close可能让晚到write误写复用fd。详见[优雅关闭](GRACEFUL_SHUTDOWN.md)。

net.eventfd_wakeup和net.eventfd_wakeup_failures保留聚合/饱和/flags/fd0/有限预算、epoll就绪、EINTR/
短读写/创建/通知/部分drain失败及单次close诊断。原fd交接专属并发/组合测试退役；真实Completion多发布者、
错误保源及晚通知屏障由concurrency.completion_queue_concurrency和net.completion_wakeup_*覆盖。
入口名称不代替实际构建/运行证据，不提供公平、硬实时、任意调度或性能保证。
