# 终止信号与Main事件循环

正式 `siftwing-server` 在配置解析成功后、日志和查询资源加载及任何线程创建前，
通过 `pthread_sigmask` 屏蔽 SIGINT/SIGTERM。Sub、worker 和按需创建的 Loader 继承
该掩码。Main 独占一个 `SFD_NONBLOCK | SFD_CLOEXEC` 的 signalfd，与 Listener、
跨线程停止 eventfd 一起注册到 LT epoll；没有额外的信号线程或异步 handler。

一次 epoll 批先检查控制来源，再决定是否 accept。同批信号和监听就绪时不再接入；
第一条受管信号产生 `MainReactorStop::shutdown_requested` 和 `signal_number`。
后续合法 `run_once` 返回同一请求，不再 wait/read/accept。信号请求保持目标队列开放，
不会提前撤销 completion 目标；跨线程立即 stop、目标关闭及 I/O 故障仍沿原故障路径。

`OnlineServer::run_once` 对正常信号返回空 error_code，创建线程须同时观察
`shutdown_signal()`（0 或首个 SIGINT/SIGTERM），再自行选择
`shutdown(timeout_ms)` 的网络排空期限。正式入口跳过后续周期工作，使用原
`--shutdown-ms`。`--run-ms` 到期也沿同一[优雅关闭](GRACEFUL_SHUTDOWN.md)流程，
正常信号不记作 `server_failed`，退出码和最终统计格式保持。

## 库调用者与所有权

`MainReactorConfig::termination_signals` 默认 false。通用 Main/OnlineServer 不修改
调用者信号掩码；显式启用前，调用者须保证所有可能接收进程信号的线程已经屏蔽两信号并保持。
适配层仅能预检当前线程，不能证明其他线程正确；在线程启动后补屏蔽主线程不足以满足合同。
接口与有限读取边界见[适配层](../include/siftwing/net/signalfd_source.hpp)、
[Main](../include/siftwing/net/main_reactor.hpp)和
[OnlineServer](../include/siftwing/net/online_server.hpp)。

Main 在所属线程注销、关闭自己的 signalfd 一次，不交给 Sub/worker。读取最多64次，
包括 EINTR；EAGAIN 表示暂时读空，预算耗尽返回明确状态，短读、EOF、非受管记录返回 EIO。
系统失败保留原错误，构造或注册失败按 RAII 回滚，不关闭借用目标。标准信号可合并，
记录数不是发送次数。

正式进程直到退出都保持两信号屏蔽，包括启动失败和正常关闭。关闭 signalfd 不清除
pending 信号；自动恢复掩码可能触发默认终止，因此入口不使用恢复掩码的析构器。
通用库的屏蔽/恢复策略由宿主负责，必须考虑 pending 信号及所有线程的掩码。

本接口只负责 SIGINT/SIGTERM 正常终止，不提供 SIGHUP、reload、热配置或自动选版本。
[显式快照加载与发布](SNAPSHOT_PUBLICATION.md)仍由控制者调用。线程定向给 worker 的信号
不保证由 Main 消费；重复标准信号不承诺强杀。signalfd 不处理同步致命错误，亦不替代
已有 `MSG_NOSIGNAL`/SIGPIPE 策略。网络期限不取消已接受的加载或查询，不是总退出硬限。

## 验证入口

`net.signalfd` 使用真实进程信号、TCP和可控系统错误，验证 mask、继承、fd0、有限读取、
同批优先、单调请求、错误优先、失败回滚及 DEL/close。
`net.signalfd_online` 验证执行中/排队查询和暂停中的真实加载遇信号后，仍沿原排空、Loader
及 worker join、Sub回收屏障收口，加载完成不自动发布。
`net.signalfd_process` 检查真实服务所有线程的 `/proc` 掩码、退出及日志，并用同一入口源码
的测试链接副本注入启动前 pending、屏蔽/创建/注册失败和关闭时 pending 窗口。
既有 `net.graceful_process` 保留 SIGINT/SIGTERM/定时退出与网络回归。
包装测试只证明所注入边界，不代表真实资源耗尽、任意调度或性能保证。
