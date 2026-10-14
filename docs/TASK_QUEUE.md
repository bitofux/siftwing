# 有界 TaskQueue

[TaskQueue<T>](../include/siftwing/concurrency/task_queue.hpp)是独立的多生产者/多消费者
拥有型 FIFO，CMake 目标 `siftwing_task_queue` 声明 C++17、项目编译选项及 `Threads::Threads`。
它只交接 `std::unique_ptr<T>`，不创建线程、不执行任务、不访问 Connection 或 fd。
固定worker执行已由独立[WorkerPool](WORKER_POOL.md)复用本队列；独立[SearchTask/Completion](SEARCH_TASK.md)已有拥有载荷，结果发布由独立[CompletionQueue](COMPLETION_QUEUE.md)提供归属和容量合同；正式网络业务接入仍待后续。

## 容量、提交与拥有关系

构造需要正容量，一次性预分配固定空槽。0 抛 invalid_argument，超过 vector 可表示范围
抛 length_error，分配失败抛 bad_alloc。容量计算尚未取出的条目，不包括消费者已经取出但
仍在执行的任务；不能据此推断任务字节、全局 RSS 或在途任务数有界。

T 必须完整且析构不抛，可以不可复制、不可移动。队列只移动默认 deleter 的 unique_ptr；
生产者提交前构造/写入任务，消费者取得后独占读取/执行/销毁。没有共享任务内部数据的同步承诺。

| API | 合同 |
|---|---|
| try_submit(unique_ptr<T>&) | 同锁依次判断 closed、invalid_task、full、accepted；仅 accepted 消费源，锁内 move 是提交点，锁外 notify_one |
| try_pop | FIFO 取出最早拥有者；开放或关闭的空队列均返回空指针，不等条目 |
| wait_pop | 等待 size 非 0 或 closed；关闭后仍继续 FIFO 排空，只有 closed 且 empty 时返回空指针 |
| close | 同锁幂等关闭提交入口，不清空/取消任务，锁外 notify_all；不等待调用者或线程退出 |
| status | 同一锁内按值复制 capacity/size/closed；不预留容量或授予内部任务借用 |

try_submit 不等待容量，但可能等待 mutex；不是无锁、无等待或硬实时接口。
拒绝及提交前加锁失败保留精确源指针和任务；不接受临时 unique_ptr，调用者据结果决定重试或释放。
正常队列操作不分配队列内存、不执行任务或析构用户任务；任务分配属于生产者。
锁失败可抛 system_error，条件等待系统错误也可能抛；无法重新持锁时标准库可能 terminate。
这些不可恢复同步边界不冒充普通 full/closed 结果。

## 谓词等待与关闭

condition_variable 的通知只提示检查谓词，不是任务计数。提交、取出和关闭使用同一个 mutex；
wait_pop 在锁内检查真实 size/closed，支持通知先到、合并和虚假唤醒。
多消费者的 FIFO 只指提交/移除顺序，不保证任务执行开始或完成顺序，也不保证线程公平性。

控制者的正常停止顺序是：

1. close 关闭入口，拒绝新任务并唤醒所有等待消费者。
2. 撤销生产者入口并等待已经进入的全部调用返回，包括锁外 notify。
3. 消费者排空已有任务，在 wait_pop 得到空指针后退出；控制者 join 所有消费者。
4. 所有调用者静止后销毁 queue。

closed/empty 只描述队列，不能证明所有调用者已返回或任务已执行完。
析构不自动 close、唤醒或 join；仅在无活跃调用者时在析构线程销毁残留任务，不能替代 worker 排空协议。
任务执行异常、取消和结果回投策略属于后续调用方，本组件不携带执行回调。

容量 2 的例子：提交任务 101、102 后，103 遇 full 仍由源拥有；消费者取 101 释放一个槽，
103 重试 accepted。close 保留 102、103，消费者仍取完二者，随后 wait_pop 返回空指针。

## 验证与边界

真实 CTest：`concurrency.task_queue`、`concurrency.task_queue_concurrency`、
`concurrency.task_queue_failures`；Release 使用运行检查，不依赖 assert。
独立 deque 参考验证容量 1/3/7 的 FIFO 与跨环尾重填，核源身份、关闭排空、通知先到和锁外任务销毁。
Linux `/proc/self/task/<tid>/wchan` 观察消费者确实停在 futex 等待后，再验证一次提交唤醒与 close 广播；
此测试依赖当前 Linux 对自身线程等待位置的可观测性，不是生产接口或跨平台承诺。

并发测试核 MPMC 任务精确消费、非原子 payload 发布可见性、单消费者的每生产者提交顺序、
提交/关闭竞争及状态容量界限。测试专属链接 wrap 注入初始槽分配和初次 mutex 锁失败，
并观察正常队列操作没有分配或析构任务；不声称拦截 DSO 内部 condition_variable wait 或真实 OOM。
ASan/UBSan 与独立 TSan 为专项工具，测试不证明任意调度、公平性、吞吐、业务执行或完整服务关闭。
