/*
 * PROJECT : SIFTWING
 * FILE    : record_format.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 定义拥有型文档的版本化字节编码、单记录校验和同步流写入合同
 */
#pragma once

#include "siftwing/base/result.hpp"
#include "siftwing/document/document_record.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace siftwing::document {

/** @brief 磁盘记录格式版本；与 DocumentId、指纹版本及快照格式版本独立。 */
inline constexpr std::uint16_t record_format_version = 1;
/** @brief 固定头的字节数；全部长度和文档库偏移均以字节计。 */
inline constexpr std::uint16_t record_header_bytes = 40;

/** @brief 调用方显式提供的两个正预算；不是进程内存硬限制。 */
struct RecordFormatLimits final {
    /** 每个字符串的最大字节数；不按 Unicode 字符计数，包括来源元数据。 */
    std::uint64_t max_field_bytes;
    /** 完整单记录最大字节数，包含头、六个长度前缀及载荷。 */
    std::uint64_t max_record_bytes;
};

/** @brief 成功校验或写入的拥有型身份与尺寸；不持有字节、流或文件偏移。 */
struct RecordFormatInfo final {
    /** 完整 uint64 值域的显式文档身份；不证明库内唯一性。 */
    base::DocumentId doc_id;
    /** 完整记录字节数；最小为 88，不包含相邻记录。 */
    std::uint64_t byte_size;
};

/**
 * @brief 按 docs/RECORD_FORMAT.md 的版本 1 格式编码一篇文档
 *
 * @param[in] record
 *     调用期间借用且不可并发修改；原对象不变。字符串逐字节保留，不验证 UTF-8、路径、URL
 *     或日期。空字符串、NUL 及缺失/已提供空元数据均可表示；来源枚举显式映射，未知值拒绝。
 * @param[in] limits
 *     两项必须为正；完整长度受 uint64、size_t、string::max_size 及预算共同限制。
 *
 * @return
 *     成功拥有完整编码字符串；失败为拥有型诊断，无部分编码。全部长度检查先于载荷分配。
 *
 * @throws
 *     分配或其他未知异常上传；不把资源耗尽伪装为格式失败。
 * @note 仅编码当前记录，不排序、不去重、不分配身份、不发布文档库。同对象只读调用需输入稳定。
 */
[[nodiscard]] base::Result<std::string> encode_record(
    const DocumentRecord &record, const RecordFormatLimits &limits);

/**
 * @brief 验证恰好一个完整记录的结构、长度和 CRC-32/ISO-HDLC
 *
 * @param[in] bytes
 *     调用期间借用的稳定字节；拒绝截断及尾随字节，不读取文件、不重建 DocumentRecord。
 * @param[in] limits
 *     与编码相同的正预算；先限制整体长度，再遍历字段与校验码。
 *
 * @return
 *     成功返回拥有型身份和完整字节数；失败无部分结果。CRC 检测意外损坏，不提供真实性保证。
 *
 * @throws
 *     诊断分配及未知异常上传。
 * @note 不验证文本语义或多记录一致性；偏移表与随机读取由document_store层提供。
 */
[[nodiscard]] base::Result<RecordFormatInfo> validate_record(
    std::string_view bytes, const RecordFormatLimits &limits);

/**
 * @brief 先完整编码，再同步向借用的输出流写入一个记录
 *
 * @param[in,out] output
 *     调用方拥有的二进制输出流；同一流需由调用方串行访问。不开关文件、不修改异常掩码，
 *     不 flush、不回滚、不重试失败写入；调用期间流与缓冲区必须存活。
 * @param[in] record
 *     同 encode_record；不保存引用。
 * @param[in] limits
 *     同 encode_record；另在写入前验证 std::streamsize 可表示完整长度。
 *
 * @return
 *     成功表示流接受全部记录字节，返回身份/尺寸；不证明 flush、close 或持久化成功。
 *     编码/长度失败不触碰流，既有坏状态及写入失败返回 record.write 诊断，失败可能留下任意前缀。
 *
 * @throws
 *     写阶段 std::ios_base::failure 转为 Result；分配与其他逃逸异常上传。
 * @warning 任意写入失败或异常后，调用方必须废弃整份临时输出，不能在残留前缀后继续拼接。
 *     最终 flush/close 的失败同样须废弃；临时文件的创建、关闭、删除由调用方负责，发布另行实现。
 */
[[nodiscard]] base::Result<RecordFormatInfo> write_record(
    std::ostream &output, const DocumentRecord &record, const RecordFormatLimits &limits);

} // namespace siftwing::document
