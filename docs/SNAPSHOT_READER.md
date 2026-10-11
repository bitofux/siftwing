# 完整快照加载

`siftwing_snapshot_reader` 提供
`SnapshotReader::open(parent_path, expected_version, limits)`，调用合同见
[reader.h](../include/siftwing/snapshot/reader.h)。它只读加载一个明确指定的正式快照，
成功后拥有全部部件和记录；后续消费者可以使用搜索索引、独立推荐词典及按身份查找的文档。
本模块不提供查询、CLI、CURRENT选择、在线切换或mmap。

父目录必须存在、可信，目录命名和内容在调用期间稳定。Reader逐组件以
`openat(O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)` 打开父目录，再打开预期版本的规范十进制叶子。
版本0与UINT64_MAX合法，不从数值推断新旧，不扫描临时目录。拒绝空/NUL/`..`路径、
任何路径组件符号链接、非恰八个普通文件、额外文件或子目录；允许相对/绝对路径、点和多余斜杠。

加载按以下顺序完成：

1. 验证26项正预算，打开并核固定八文件目录。
2. 读取完整manifest，解析格式/身份/统计/预算；身份必须等于指定版本。
3. 核COMPLETE的精确长度和字节，绑定这份manifest。
4. 按固定角色读取六payload，核精确尺寸与SHA-256。
5. 解码偏移表，核记录库完整覆盖、每条记录CRC与身份，拥有重建记录。
6. 解码搜索词表和原TF/文档关系，复用InvertedIndex及WeightedIndex构建器验证整数统计，
   完整规范重导出并逐字节比较search-weights，包括DF/N、文档范数、IDF和归一化权重。
7. 重建独立推荐词频词典，字符索引必须恰好对应其词键的Unicode标量关系；统计分别对应manifest。
8. 再核完整目录并检查关闭全部fd，然后一次返回完整拥有对象。

文本解析拒绝未知头/版本、非规范十进制、非小写hex、错误行/字段数、CRLF/多空格/缺LF/尾随。
词表必须符合已封板词键合同，ID和关系次序必须正确，记录、偏移表和搜索文档集合一致。
推荐交付数与搜索N保持独立，空集合仍须完整编码。重新计算所有SHA和COMPLETE的格式或关系错误
也会被语义校验拒绝；哈希本身不是认证，不证明来源/config声明真实或原TF来自记录文本。

权重验证要求当前数值运行环境能够逐位复现策略1的保存字节，沿用M1.22的IEEE754 binary64、
FE_TONEAREST、无fast-math及固定计算顺序合同。跨CPU/libm可能存在差异，本模块遇到差异拒绝，
不会静默改写权重或迁移快照。原始整数TF/DF完整保留。

`SnapshotReadLimits`组合记录/库/偏移表、manifest、搜索和推荐预算，26项均为正且含端点。
manifest/尺寸预算先于载荷分配，解析根据实际行逐步建立对象，不按未核验的声明计数reserve。
六payload总字节不包含manifest和COMPLETE；后者按生成规范的固定精确长度读取。
全内存载荷、拥有记录、临时TF表及重建索引有额外成本，预算不是内存或CPU硬限制。

短读/EINTR重试，提前EOF、其他IO错误、观察到尺寸/时间/身份变化均拒绝；close一次不重试。
所有临时句柄由RAII释放，普通失败仅返回诊断，无部分Reader；分配/未知异常上传。
Reader从不写入、删除或清理产物，调用结束不保留fd，加载完成后原文件的生命不会影响已有对象。
稳定父目录前提不提供敌对并发写者、网络文件系统或全部改写时序下的原子快照保障。

Reader不可复制，noexcept移动转交唯一State，源为空，自移动保持。const左值getter返回借用指针；
合法空快照仍有完整部件，移空对象的getter返回nullptr。`document(id)`对排序记录二分查找，
未知ID返回nullptr。禁止从临时对象取得借用；所有借用在移动/赋值/析构后失效，不能并发改变所有者。

诊断context为reader.limits/path/open/stat/directory/bytes/read/close/version/syntax/semantic，
或复用record/store/lexicon/index/weights/dictionary/manifest诊断。生产目标无新第三方库；
测试包含独立Python struct/zlib/hashlib与数学/Unicode生成器，以及真实Writer接线和GNU ld IO故障包装。
