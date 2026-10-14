# eventfd 新连接唤醒

独立组件的公共合同在[头文件](../include/siftwing/net/eventfd_wakeup.hpp)。
它将[fd移交队列](FD_HANDOFF_QUEUE.md)的真实数据与通知分开，提供提交后通知组合函数。
真实epoll消费者测试验证协议；现有[Sub Reactor](SUB_REACTOR.md)的连接和队列接入仍属后续集成。

`create_eventfd_wakeup()`单次创建普通聚合eventfd，带NONBLOCK/CLOEXEC，不用SEMAPHORE。
成功返回UniqueFd，错误返回generic errno；创建EINTR不重试。fd 0也是有效拥有者。

| 接口 | 结果与调用方责任 |
|---|---|
| `notify_eventfd(fd)` | 写8字节的1；EINTR最多64次尝试，耗尽报告EINTR；饱和EAGAIN视作已有合并通知；短写EIO |
| `drain_eventfd(fd, budget)` | 成功read和EINTR均占预算；观察EAGAIN为drained，耗尽为budget_exhausted，其他错误为failed；保存成功read次数，忽略计数值 |
| `try_handoff_and_notify(queue, source, fd)` | 先入队，只有accepted通知；返回入队结果和通知诊断。accepted后源为空，通知失败也不收回、不重投、不由生产者关闭 |

上述fd必须是稳定借用的非阻塞普通eventfd；组件不检测fd类型，不能将EAGAIN语义推广至文件或管道。
负fd预检EBADF；drain零预算EINVAL。通知和drain无分配/异常、不保持errno；组合函数可在提交前抛mutex错误。
通知成功不保证消费者已运行；drained只表示当次观察无通知，不证明队列为空。

消费者每轮先有限清通知，再从真实队列取有限批次。队列尚有条目时继续处理，不直接无限等待。
先清通知可以避免“先取队列，后清掉新发布者的通知，再睡眠”的漏唤醒窗口；队列锁提供数据可见性，eventfd不替代锁。
预算耗尽允许下一轮再次检查；计数可合并、饱和，也可能来自停止通知，不能决定pop次数。

```cpp
auto result = try_handoff_and_notify(queue, source, wakeup_fd);
if (result.handoff == FdHandoffResult::accepted) {
    // source已为空；若notification_error非空，诊断并协调目标消费者，不能重投。
}
// 目标消费者：清通知后最多取调用方预算；仍非空时继续处理。
auto notification = drain_eventfd(wakeup_fd, 64);
for (std::size_t i = 0; i < 16; ++i) {
    auto item = queue.try_pop();
    if (!item) break;
    // 拥有型item已在锁外：后续注册或显式close。
}
```

eventfd由目标消费者创建、注册和最终关闭，生产者只借用稳定数字。
停止顺序是关闭队列入口、撤销发布入口、确认所有已进入的完整“入队+通知”调用返回，
最后控制通知也返回并撤借用，然后目标消费者清理队列、注销、关闭eventfd，控制者join。
closed/empty都不证明发布者静止；过早关闭会让迟到write误写复用的fd。
通知失败须有显式协调策略；有限epoll等待可作为停止后备，本组件不自动恢复服务或建立后台重试。

真实CTest为`net.eventfd_wakeup`、`net.eventfd_wakeup_concurrency`、`net.eventfd_wakeup_failures`。
覆盖聚合/饱和/flags/fd 0/有限预算、即时epoll可读与清除、多发布者恰好一次消费及停止静止门禁，
并以test-only链接包装注入EINTR、短读写、创建/通知/部分drain失败、加锁异常和一次close诊断。
测试不依赖assert，Release仍执行。未声明Main/Sub生产服务集成、吞吐、公平性、硬实时或任意调度正确性。
