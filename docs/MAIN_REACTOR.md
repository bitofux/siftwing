# Main Reactor 轮询分配

独立[MainReactor接口](../include/siftwing/net/main_reactor.hpp)在所属I/O线程拥有Listener、LT epoll与自己的停止eventfd。
调用方在该线程create、重复run_once、close和析构；组件不创建线程，不读取或发送客户端业务数据，不构造Connection。
它组合[监听接入](LISTENER.md)、[fd队列](FD_HANDOFF_QUEUE.md)和[eventfd通知](EVENTFD_WAKEUP.md)。
旧Sub生命周期和单Loop保持；[多Sub协议集成](MULTI_REACTOR.md)使用独立ProtocolSubReactor组合Main，验证Connection固定归属。
本文的三个Main单元测试仍仅验证交接协议；worker与正式搜索服务命令的组合见[在线服务](ONLINE_SEARCH.md)。

调用方提供1—64个固定目标，每个包含稳定的queue指针与普通非阻塞eventfd数字。
全部目标必须在创建前完整就绪且开放，彼此queue与fd数字不能重复，默认不会按负载重选或动态缩容。
本层不验证eventfd类型，不从数字推断就绪，也不获知Sub的运行首因；控制者须汇总失败并停止Main。
目标借用一直保持到Main所有发布+通知返回，最后控制通知也返回/撤借用后才允许consumer关闭通知端。

| 接口/结果 | 合同 |
|---|---|
| `create(targets, listen, config)` | 完整结构预检后建监听、epoll与停止端，再注册两端；成功才交付unique_ptr；配置非法EINVAL、目标关闭ECANCELED；分配/预检加锁可抛 |
| `run_once(timeout)` | timeout为0—100ms，禁止无限等待；单次wait，EINTR返回interrupted；accept_attempt_budget为1—1024，含EINTR/可重试网络错误 |
| 队列移交 | 每次选定目标的移交尝试推进游标；accepted消费源、通知失败仍计handed_off；full在Main关闭源、不改投且继续下一连接 |
| 致命错误 | 目标关闭、socket配置/接入资源/通知/同步异常停止整体Main；不静默缩容，不忙重试；已经提交的fd留给目标 |
| `request_stop()` | 可跨线程，先置原子停止标志再有限通知自己的eventfd；失败仍保留停止，100ms等待为后备；不是退出/join完成 |
| `close()` | 所属线程尝试关闭各专用目标发布入口，DEL/close自己的资源；不pop或close已提交条目，不关闭目标eventfd，不join目标 |

每轮满足`accepted == handed_off + rejected`，通知失败仍属于handed_off；无目标客户端fd对外借用。
固定TCP_NODELAY默认关闭配置操作，启用时对每个新fd单次设置；配置失败在移交前由Main关闭源并终止。
accept复用Listener已有原子NONBLOCK/CLOEXEC、有限EINTR与Linux pending network error重试合同。
预算耗尽不证明监听队列为空；LT下下一轮继续。队列满不保证负载平衡，组件不承诺相等连接存活数或CPU利用率。

自身停止eventfd的notify与撤销用一个mutex串行；Main在锁内移动拥有者到局部，锁外注销/close。
迟到的stop调用只见空拥有者，不写入复用fd。此保护不延长Main对象生命期；析构前仍必须撤销stop入口并等调用者返回。
create失败只回收新建自身资源，目标原条目和开放状态保持；运行失败关闭目标发布入口以协助控制者停止。
目标mutex清理错误记录后继续其他清理，失败入口可能仍开放且重复Main close不重试它，须控制者协调关闭；自身同步异常可抛，析构同步异常terminate。首因和清理诊断分别保存，Linux close从不重试。

最小调用方式（由所属I/O线程执行；targets资源由外部控制者保证生命期）：

```cpp
auto created = MainReactor::create(targets);
if (auto* error = std::get_if<std::error_code>(&created)) {
    // 报告启动失败；目标尚未被Main关闭，控制者协调停止。
} else {
    auto main = std::move(std::get<std::unique_ptr<MainReactor>>(created));
    for (;;) {
        auto step = main->run_once(100);
        if (step.stop == MainReactorStop::closed || step.stop == MainReactorStop::failed) break;
    }
    // 撤销并等待所有request_stop调用后，在当前线程销毁main。
}
```

CTest注册`net.main_reactor`、`net.main_reactor_concurrency`、`net.main_reactor_failures`。
真实TCP字节由目标独立读取，按独立modulo参考核轮询、满队列不改投/游标前进、LT预算续调度和原子flags。
多目标消费者自行拥有epoll/eventfd并完成恰好一次消费；线程控制者确认Main和最后通知返回后才关闭目标端。
test-only链接包装覆盖启动各阶段、分配/锁失败、资源错误、通知错误、关闭竞态、一次close诊断及停止通知失败的真实有限wait。
Release测试不依赖assert。本组件三测试不单独证明多Sub协议链，相应组合证据见[MULTI_REACTOR](MULTI_REACTOR.md)；尚未验证性能、公平性、硬实时或任意调度。
