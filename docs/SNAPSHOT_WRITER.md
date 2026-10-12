# 完整快照写入与发布

`siftwing_snapshot_writer` 提供 [write_snapshot](../include/siftwing/snapshot/writer.hpp)。
它组合已有文档记录/偏移表、搜索权重、独立推荐词典及 [manifest v1](SNAPSHOT_FORMAT.md)，
保持已有格式与所有权合同；不加载分词资源，不重新分析正文或去重，没有新第三方依赖。

调用方同步提供搜索保留记录、有效 `WeightedIndex`、有效 `RecommendationDictionary`、
`SnapshotMetadata` 和 `SnapshotWriteLimits`。记录数及唯一 DocId 集合须与搜索文档统计完全一致，
包含显式空篇；输入排列不影响输出，记录库及偏移表按 DocId 升序生成。
TF 是否来自该正文、来源摘要是否如实描述输入/config，仍由上游保证。
推荐交付数、累计频次及局部词ID独立于搜索，不能用搜索去重结果代替推荐累计。

Writer 内部生成三个新规范文本 payload：搜索词表、推荐词频词典和 Unicode 标量字符索引。
它们使用既有部件已验证的排序/身份/统计，词键为原 UTF-8 字节的小写 hex。
记录和偏移表复用已有编码，权重复用 `WeightedIndex::serialize`；不序列化 C++ 对象内存。
完成六个 payload 后派生各自长度/SHA-256及两支统计，严格生成 manifest 和 COMPLETE。
所有版本及字节布局均遵守 SNAPSHOT_FORMAT.md，没有自动迁移或新版本。

八项正预算组合既有五项 `DocumentStoreLimits` 与三项 `ManifestLimits`，含端点。
编码逐追加限制单文件及总预算剩余容量；文档数/单字段/单记录/库/偏移表分别受检。
六 payload 总预算不含 manifest/COMPLETE，清单单独限完整字节。
预算不包含输入装载、容器容量、指针排序、分配器和文件系统元数据，不是硬进程资源限额。
局部保存完整六文件及清单/标记字节，内存随产物总量增长；超大语料的流式写入留后续设计。

输出父目录必须已经存在、可信且调用期间稳定。非空路径不得含NUL或 `..` 组件；
绝对/相对路径、重复斜杠与 `.` 组件允许。以 `openat` 逐组件打开目录并使用 NOFOLLOW，
拒绝中间及叶子符号链接，不自行创建父目录，也不从记录来源路径派生输出。
实际写入始终相对于已打开的父/临时目录 fd；可信父目录及本次临时内容不得被外部改写。
这些条件不提供恶意并发写者、网络文件系统或路径被外部重命名下的完整安全保证。

执行顺序如下：

1. 检查正预算、路径词法、文档数/身份，生成并验证规范字节，尚无文件写入。
2. 打开父目录并拒绝任何既有版本对象；创建
   `.siftwing-<SnapshotVersion>-<128位随机hex>.tmp`，目录0700，最多尝试8次随机冲突。
3. 固定顺序创建六 payload，使用 O_EXCL/NOFOLLOW/CLOEXEC、0600；完整写入后 fsync，
   检查 close，再重新只读打开，fstat核类型/大小，pread逐字节比较，检查大小及 close。
4. 在全部 payload 通过后同样写入/回读 manifest.txt、COMPLETE；核临时目录恰为八个普通文件，
   无额外项、子目录或符号链接，fsync并检查关闭临时目录。
5. Linux `renameat2(RENAME_NOREPLACE)` 原子发布为规范十进制版本目录，再 fsync并关闭父目录。

权限受调用方 umask 收紧，Writer不会放宽权限。短读写继续，EINTR在可重试IO中重试；
零写、提前EOF、字节不符、同步或关闭错误明确拒绝。Linux close只尝试一次，先消费局部句柄，
不重试可能已被复用的fd；析构仅释放尚未消费的局部fd，分配/未知异常向上传递。
不支持 renameat2/NOREPLACE 的内核或文件系统失败，不降级为有覆盖竞态的普通 rename。
正式目录已存在时即使字节相同也拒绝，不覆盖文件、目录、链接或FIFO。

发布前普通失败返回 `Result::failure`，不会生成正式目录，不改输入或已有对象。
本次自建失败临时目录原样保留供诊断，message包含准确临时叶子；异常也保留残留，
可按上述命名定位。目录中的 COMPLETE 不能把失败残留视为正式快照，消费者只能选择正式版本。
Writer没有自动清理、重试继续、回滚或删除旧版本策略；调用方须另行安排失败目录处置。

rename是唯一提交点。返回载荷及Result在提交前完整构造，成功后只做不分配的系统调用、
标量状态更新和 noexcept 移动。`PublishedSnapshot` 始终表示正式目录已经发布：

| durability | system_errno | 含义与调用方动作 |
|---|---|---|
| synced | 0 | 文件、临时目录及父目录同步/检查关闭均成功，可将本次发布视为同步完成 |
| unconfirmed | 首个父目录fsync/close的errno | 已发布，但持久性未确认；正式目录保留，不能把它当未发布而盲目重试或删除 |

fsync成功依赖当前Linux/文件系统行为，不等于实测断电恢复。原子目录可见性与持久性分别维护。
两个独立进程竞争同一版本时，NOREPLACE至多允许一个提交；失败者保留自己的临时目录。
输入和所有者不得与调用并发修改/移动/析构，没有后台线程、缓存或跨线程借用。
成功结果拥有实际 Manifest，复制独立；默认移动后的成员有效但内容未指定，没有任何fd。

诊断 context 为 writer.limits/path/documents/identity/bytes/exists/random/mkdir/open/stat/
write/sync/close/read/directory/rename，或沿用 record/store/weights/manifest 上下文。
SHA-256检测意外损坏，不是认证或来源真实性证明。当前 Writer从有效部件派生文件，并核回读字节，
不提供从任意目录解码并验证跨文件语义的 SnapshotReader，也不维护CURRENT指针、在线热切换、
查询或CLI。所有持久化/路径条件只适用于调用方选定的新输出父目录，不访问原始私有语料。

正式离线应用[siftwing-indexer](INDEXER.md)分别组装搜索保留集和推荐累计，产生本格式的完整快照；
组件的旧API、预算/所有权、来源声明与发布/加载边界保持。正式本地SEARCH/SUGGEST入口由后续模块提供。
