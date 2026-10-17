# epoll 创建与 LT 注册合同

已有[fd/socket基线](SOCKET_BASELINE.md)负责资源和单次收发；本层提供Linux epoll创建和注册，
供所属I/O线程使用，已有[单Loop](PROTOCOL_LOOP.md)与[多Sub](MULTI_REACTOR.md)调用方。接口权威为`include/siftwing/net/epoll_ops.hpp`，目标`siftwing_epoll`
实现复用`siftwing_socket`。本层没有事件循环、等待封装、Listener、Connection或业务分发。

## 创建、借用与标识

`create_epoll()`调用一次`epoll_create1(EPOLL_CLOEXEC)`，成功分支为独占的`UniqueFd`，
失败为`generic_category()`下的原errno；包括EINTR，不重试或通过fcntl补标志。
fd 0合法，移动/释放/关闭沿用`UniqueFd`合同。epoll拥有内核注册集合，不拥有被注册的目标fd。
销毁epoll会释放集合和自己的fd，目标fd由原拥有者继续管理。

`epoll_add(epoll_fd, fd, interests, token)`、`epoll_modify(...)`及`epoll_remove(epoll_fd, fd)`
在调用期间借用稳定的fd，最多调用一次`epoll_ctl`。由所属I/O线程或调用方同步执行，
本层不持有回调、连接指针、注册镜像或锁；所有接口noexcept、自身无分配，不承诺保持errno。

token是完整`uint64_t`值，包括0和最大值，复制到`event.data.u64`。它不携带所有权，也不是
可直接解引用的指针。上层负责映射稳定身份、检查generation并处理已关闭对象，现有Sub实现见[连接身份](CONNECTION_IDENTITY.md)。

## LT 掩码与错误

支持关注位为`EPOLLIN|EPOLLOUT|EPOLLRDHUP`的任意子集，0允许暂停普通读写通知。
`EPOLLERR`和`EPOLLHUP`由内核自动报告，无需订阅；输入显式包含这两位仍按不支持位拒绝。
ET、ONESHOT、EXCLUSIVE、WAKEUP、PRI和其他位返回EINVAL，不进入内核。
LT就绪在条件仍满足时可再次出现；事件不是消息边界或完成承诺，收发仍须处理短IO/EAGAIN。
EPOLLOUT应由上层在有发送余量时关注，长期开启可引发忙循环；本层不自动管理写事件开关。

| 操作 | 成功 | 失败与上层责任 |
|---|---|---|
| ADD | 新增完整掩码和token | EEXIST明确返回，不自动改为MOD；无部分注册或目标清理 |
| MOD | 完整替换掩码和token | 失败保持旧注册，ENOENT不自动改为ADD；不是合并关注位 |
| DEL | 注销原目标，event参数nullptr | ENOENT/EBADF等明确返回，不假装重复清理成功、不关闭fd |

负epoll/目标fd优先返回EBADF；ADD/MOD的无效掩码其次返回EINVAL，均不调用内核。
其他错误交由Linux检查，原errno保持在error_code中，包括EPERM、ENOSPC、ENOMEM、EINTR。
一次失败没有隐藏补救调用；上层决定诊断、按预算重试或终止。

## 关闭与已取事件

正常清理按DEL→关闭目标fd进行。Linux注册键同时关联注册时的fd数字与open file description。
若有dup别名，关闭原数字后注册可能继续存活；对别名DEL并不等于注销原数字的注册。
只有最后一个指向该open file description的fd关闭后才自动移除；不能用复用后的整数指认旧连接。

MOD不会改写已取出批次里的旧token，DEL也不会清除上层已经保存的事件。
上层必须检查对象是否仍有效，不能从注销成功推出“不会再处理旧事件”。本层没有引入
generation存储、事件分发或连接生命周期机制。

## 验证与限制

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^net.epoll_'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^net.epoll_'
```

`net.epoll_ops`用真实socketpair及内核等待验证8种关注位子集、LT重复、64位token与零token、
MOD替换/失败保持、DEL/重复操作、RDHUP与pending→EOF、零掩码下HUP、销毁不关闭目标、
最后关闭自动清理/dup别名注册存活和隔离子进程fd0。`/proc/self/fd`计数核本次释放。

`net.epoll_failures`仅测试链接`--wrap=epoll_create1,epoll_ctl`，验证原errno、标志、完整参数、
单次调用、预检不调用和失败后原内核注册/目标fd保持；生产库没有hook。
显式检查在Release NDEBUG下仍有效。raw epoll_wait及dup只为测试夹具，不提供生产等待/别名管理。
有限注入不证明真实资源耗尽/信号时序、任意并发、ET、长期服务、业务或性能。
