# 文档库记录格式 v1

本格式保存抽取后的 `DocumentRecord`，不是原始文件或搜索 TF。文档库的字节内容由完整记录
顺序拼接；本记录组件提供单记录编码、校验和同步流写入。[偏移表与随机读取](DOCUMENT_STORE.md)
另行检查库内身份唯一及完整覆盖；文件级清单见[快照格式](SNAPSHOT_FORMAT.md)，发布与加载见[Writer](SNAPSHOT_WRITER.md)和[Reader](SNAPSHOT_READER.md)。各记录保留调用方顺序，
不生成或重排 DocId。

所有整数使用固定宽度无符号网络字节序（高字节在前），长度和偏移单位均为字节。
禁止序列化对象内存、枚举底层值或 `std::hash`。固定头如下：

| offset | 字节数 | 含义 |
|---|---|---|
| 0 | 4 | ASCII `SWDR` |
| 4 | 2 | 格式版本，必须为 1 |
| 6 | 2 | 头长，必须为 40 |
| 8 | 8 | 完整记录长度，含头和全部字段 |
| 16 | 8 | DocId；0 与 UINT64_MAX 均合法 |
| 24 | 8 | 来源逻辑序号；0 与 UINT64_MAX 均合法 |
| 32 | 1 | 来源编码：TXT=1、RSS=2、历史 JSONL=3 |
| 33 | 1 | bit0 URL、bit1 author、bit2 published_at_raw 是否提供 |
| 34 | 2 | 保留字段，必须为 0 |
| 36 | 4 | CRC-32/ISO-HDLC |

来源编码 3 仅保留既有值容器的表示能力，不引入 JSONL 读取器。未知来源、版本、头长、
保留位或 flags 均拒绝。后续兼容性变化须另行审查及升版，不静默解释未知布局。

头后严格依次保存 title、content、source_id.input_path、url、author、published_at_raw。
每项均为 `uint64 长度 + 原始字符串字节`，六项始终存在。可选字段未提供时长度必须为 0；
已提供空值以 presence bit 区分。最小完整记录为 `40 + 6*8 = 88` 字节。
不补终止 NUL、对齐 padding 或隐式换行；字符串可含任意字节，编码层不验证 UTF-8、路径、
URL、日期或抽取完整性。文本生产合同仍由已有读取/文本层承担。

CRC 覆盖 `[0,36)` 与 `[40,total_length)` 的拼接，跳过 CRC 字段本身。
使用反射多项式 `0xedb88320`、初始值 `0xffffffff`、最终异或 `0xffffffff`，逐字节低位递推；
与 Python `zlib.crc32(header[:36] + payload)` 的无符号结果一致。CRC 包含身份、来源、
长度和字符串，用于检测意外损坏；不能抵抗同时改写字节与 CRC 的写者，不用于认证。

调用方必须显式提供正的单字段与完整记录字节预算。编码在分配载荷前检查各字段、总长、
整数回绕、size_t 及 string 容量；预算含框架开销。校验要求输入恰好是一条完整记录，
先验整体预算及头，再逐项检查可用字节和字段预算，最后校验 CRC，拒绝尾随字节。
错误 context 分为 `record.limits`、`record.header`、`record.length`、`record.checksum`、
`record.write`；Result 失败没有部分成功载荷，分配及未知异常向上传递。

`write_record` 借用调用方的二进制 `std::ostream`，完整编码成功后才调用一次 `write`。
编码失败不触碰输出；流已经失败、短写或 ios_base::failure 产生写入失败，异常掩码不改动。
函数不 flush、不 close、不重试、不回滚；成功仅表示流接受全部字节。调用方必须使用
独立临时输出，并检查最终 flush/close。任意写入、flush、close 失败或异常后，关闭并
废弃整份临时输出，禁止发布或在损坏前缀后续写。本接口不拥有路径或文件，因此创建、
关闭和删除由调用方承担；当前不是生产文件发布 API。测试用临时文件演示这项责任。
