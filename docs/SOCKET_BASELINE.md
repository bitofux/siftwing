# fd RAII 与 socket 基线

已有[codec](FRAME_CODEC.md)处理内存帧。本层提供Linux资源拥有和原始socket调用，供后续
事件循环使用，并已供[Listener](LISTENER.md)与[Connection读取](CONNECTION_READ.md)复用；
socket基线自身没有epoll、连接调度、协议处理或业务服务。

## fd 所有权

`UniqueFd`接管独占的非负fd，0合法，负值统一为空的-1。不可复制，移动后源为空，
移动赋值先关闭旧资源；自移动保持。`get()`仅允许左值借用，借用数字不得由外部关闭，
使用/移动/关闭须串行化；移动、release、close或销毁使原借用失效。
`release()`不关闭，接收者负责最终关闭或交给另一个唯一拥有者。

Linux的`close()`先释放fd槽，再可能报告错误；`UniqueFd::close()`先清空自己，再调用一次，
即使EINTR也不重试，否则复用的数字可能指向另一资源。显式调用返回保存原errno的
`std::error_code`，析构与移动赋值忽略关闭错误；这些关闭操作保持调用前errno。
这只是Linux合同，不是所有POSIX平台的close策略。对象不检查接管数字是否已打开，
不dup、不shutdown，也不能修复两个拥有者接管同一fd的错误。

## 创建与单次收发

接口权威为`include/siftwing/net/unique_fd.hpp`及`socket_ops.hpp`；CMake目标`siftwing_socket`
PUBLIC依赖`siftwing_base`，无新第三方。系统错误采用`generic_category()`保存errno，
没有字符串诊断分配，与协议的`DecodeError`/`ProtocolError`保持独立。

`open_tcp_socket(IpFamily::v4/v6)`用一次socket调用创建TCP描述符，同时设置
`SOCK_NONBLOCK|SOCK_CLOEXEC`，避免先创建再fcntl的继承/阻塞窗口。结果为
`variant<UniqueFd,error_code>`，无效family返回EINVAL，其他失败保留原errno，包括EINTR与EMFILE。
无隐藏重试、fcntl退化或进程配置修改；没有bind/listen/connect封装，IPv6仍使用内核默认策略。

`receive_some(fd, destination, capacity)`和`send_some(fd, string_view)`同步借用稳定fd/存储，
最多执行一次系统调用。两者用`MSG_DONTWAIT`确保本次非阻塞，即使借入的socket本身为阻塞模式，
也不改变其描述符标志。send额外用`MSG_NOSIGNAL`抑制本次SIGPIPE，不安装signal handler或改信号mask。

| IoState | bytes/error | 上层责任 |
|---|---|---|
| progress | 实际字节数，error为空；短IO也是进展 | 按bytes推进余量，下一次调用受调度预算控制，成功不证明对端业务处理 |
| would_block | 0，EAGAIN/EWOULDBLOCK | 等待适合的就绪通知，不能忙等 |
| interrupted | 0，EINTR | 上层决定按预算重试或返回事件循环，不是连接EOF |
| eof | 0，无error，仅非空recv返回0 | 处理读取方向关闭；仍须按上层协议处理残留数据和其他方向 |
| error | 0，其他errno（如EPIPE/ECONNRESET） | 记录并决定终止；本函数不关闭fd或发送错误帧 |

负fd返回EBADF；接收非零长度空指针或长度超过ssize_t上限返回EINVAL，不进入内核。
空收发是progress/0无操作，不能因空目标缓冲区判EOF。非空send返回0仍是零进展，上层不能无限重试。
输入存储须覆盖请求范围，接口不持有view、不更新Buffer、不自动补齐，也不承诺保持收发/创建前errno；
结果字段是错误判断依据。所有接口noexcept且自身不分配，fd稳定性和并发串行化由调用者保证。

## 验证入口与边界

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^net.socket_'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^net.socket_'
```

- `net.socket_ops`：真实fd移动/release/关闭/复用/异常清理、IPv4/IPv6创建标志、socketpair阻塞fd的
  单次非阻塞收发、短IO/EAGAIN、原始二进制字节、半关闭后pending数据→EOF及反向通信、真实TCP loopback。
  SIGPIPE默认动作（子进程显式解除继承屏蔽）与fd0由隔离子进程验证；`/proc/self/fd`前后计数检查本轮资源。
- `net.socket_failures`：仅测试链接器`--wrap=socket,close,recv,send`注入errno/短IO，证明调用一次、
  close报告EINTR/EIO后数字复用仍不被重关。短IO实际转发更短范围到内核，不只篡改返回量；生产库无hook。
- `net.socket_compile_contract`：真实编译合法移动/转交与禁止复制/临时get，使用当前构建编译器及C++17。

测试使用显式检查，Release NDEBUG不会取消；loopback中的bind/listen/connect/accept4/poll仅为夹具，
本页测试只验证socket基线；后续已有[epoll注册](EPOLL_REGISTRATION.md)、Listener与Connection读取。
TCP夹具累计实际短IO，不把一次recv视为一个完整消息。
故障注入证明分支策略，不冒称真实信号时序或系统资源耗尽；fd计数只证明本次观察。
未验证任意并发、真实EMFILE/OOM、RST时序、网络故障/吞吐/跨平台、codec接线或业务请求。
EOF是读取方向状态，send成功只表示内核接受字节；以上测试不证明完整网络服务已实现。
