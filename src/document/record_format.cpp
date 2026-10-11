/*
 * PROJECT : SIFTWING
 * FILE    : record_format.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 以显式网络字节序、受检长度和完整记录 CRC 编码，不序列化 C++ 对象内存
 */
#include "siftwing/document/record_format.h"
#include "siftwing/base/checked.h"

#include <array>
#include <ostream>
#include <utility>

namespace siftwing::document {
namespace {
using base::Result;
template <typename T> Result<T> fail(const char *message, const char *context) {
    return Result<T>::failure({message, context});
}
bool valid_limits(const RecordFormatLimits &limits) {
    return limits.max_field_bytes != 0 && limits.max_record_bytes != 0;
}
// 仅在已证明目标区域完整存在后使用；不按本机对象布局写整数。
void put(std::string &bytes, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        const auto octet = static_cast<unsigned char>((value >> (8U * (width - i - 1U))) & 0xffU);
        // 经 unsigned char 别名访问保留 0x80—0xff，避免超范围 char 转换依赖实现。
        reinterpret_cast<unsigned char *>(bytes.data())[offset + i] = octet;
    }
}
std::uint64_t get(std::string_view bytes, std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[offset + i]);
    }
    return value;
}
// CRC 字段不进入递推；其余头字段和全部载荷均受保护，身份损坏也不能静默通过。
std::uint32_t checksum(std::string_view bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i >= 36 && i < 40) { continue; }
        crc ^= static_cast<unsigned char>(bytes[i]);
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    return crc ^ 0xffffffffU;
}
unsigned kind_code(SourceKind kind) {
    switch (kind) {
    case SourceKind::txt: return 1;
    case SourceKind::rss: return 2;
    case SourceKind::jsonl: return 3;
    }
    return 0;
}
} // namespace

Result<std::string> encode_record(const DocumentRecord &record, const RecordFormatLimits &limits) {
    if (!valid_limits(limits)) { return fail<std::string>("budgets must be positive", "record.limits"); }
    const auto kind = kind_code(record.source.kind);
    if (!kind) { return fail<std::string>("unknown source kind", "record.header"); }
    const auto &source = record.source;
    const std::array<std::string_view, 6> fields{
        record.title, record.content, source.source_id.input_path,
        source.url ? std::string_view{*source.url} : std::string_view{},
        source.author ? std::string_view{*source.author} : std::string_view{},
        source.published_at_raw ? std::string_view{*source.published_at_raw} : std::string_view{}};
    std::uint64_t total = record_header_bytes;
    for (const auto field : fields) {
        const auto size = base::checked_narrow<std::uint64_t>(field.size(), "record.length");
        if (!size) { return Result<std::string>::failure(size.error()); }
        if (size.value() > limits.max_field_bytes) {
            return fail<std::string>("field exceeds byte budget", "record.length");
        }
        const auto prefix = base::checked_add(total, std::uint64_t{8}, "record.length");
        if (!prefix) { return Result<std::string>::failure(prefix.error()); }
        const auto next = base::checked_add(prefix.value(), size.value(), "record.length");
        if (!next) { return Result<std::string>::failure(next.error()); }
        total = next.value();
    }
    if (total > limits.max_record_bytes) {
        return fail<std::string>("record exceeds byte budget", "record.length");
    }
    const auto size = base::checked_narrow<std::size_t>(total, "record.length");
    if (!size) { return Result<std::string>::failure(size.error()); }
    if (size.value() > std::string{}.max_size()) {
        return fail<std::string>("record exceeds string capacity", "record.length");
    }
    // 全部尺寸验证完再一次性分配；每次推进已由总长证明不会回绕。
    std::string bytes(size.value(), '\0');
    bytes.replace(0, 4, "SWDR");
    put(bytes, 4, record_format_version, 2);
    put(bytes, 6, record_header_bytes, 2);
    put(bytes, 8, total, 8);
    put(bytes, 16, record.doc_id.value(), 8);
    put(bytes, 24, source.source_id.record_ordinal, 8);
    put(bytes, 32, kind, 1);
    const unsigned flags = (source.url ? 1U : 0U) | (source.author ? 2U : 0U) |
                           (source.published_at_raw ? 4U : 0U);
    put(bytes, 33, flags, 1);
    std::size_t cursor = record_header_bytes;
    for (const auto field : fields) {
        put(bytes, cursor, static_cast<std::uint64_t>(field.size()), 8);
        cursor += 8;
        if (!field.empty()) { bytes.replace(cursor, field.size(), field.data(), field.size()); }
        cursor += field.size();
    }
    put(bytes, 36, checksum(bytes), 4);
    return Result<std::string>::success(std::move(bytes));
}

Result<RecordFormatInfo> validate_record(std::string_view bytes, const RecordFormatLimits &limits) {
    if (!valid_limits(limits)) { return fail<RecordFormatInfo>("budgets must be positive", "record.limits"); }
    const auto size = base::checked_narrow<std::uint64_t>(bytes.size(), "record.length");
    if (!size) { return Result<RecordFormatInfo>::failure(size.error()); }
    if (size.value() > limits.max_record_bytes || bytes.size() < record_header_bytes) {
        return fail<RecordFormatInfo>("record size outside limits", "record.length");
    }
    if (bytes.substr(0, 4) != "SWDR" || get(bytes, 4, 2) != record_format_version ||
        get(bytes, 6, 2) != record_header_bytes || get(bytes, 34, 2) != 0 ||
        get(bytes, 32, 1) < 1 || get(bytes, 32, 1) > 3 || get(bytes, 33, 1) > 7) {
        return fail<RecordFormatInfo>("unsupported or invalid header", "record.header");
    }
    if (get(bytes, 8, 8) != size.value()) {
        return fail<RecordFormatInfo>("declared length differs from bytes", "record.length");
    }
    std::size_t cursor = record_header_bytes;
    const auto flags = get(bytes, 33, 1);
    for (unsigned field = 0; field < 6; ++field) {
        // 先用剩余字节做减法检查，再读取或推进；恶意 uint64 长度不会参与未检查加法。
        if (bytes.size() - cursor < 8) {
            return fail<RecordFormatInfo>("truncated field length", "record.length");
        }
        const auto length = get(bytes, cursor, 8);
        cursor += 8;
        if (length > limits.max_field_bytes || length > bytes.size() - cursor) {
            return fail<RecordFormatInfo>("field length outside available bytes or budget", "record.length");
        }
        if (field >= 3 && (flags & (std::uint64_t{1} << (field - 3U))) == 0 && length != 0) {
            return fail<RecordFormatInfo>("absent metadata has nonempty payload", "record.header");
        }
        cursor += static_cast<std::size_t>(length);
    }
    if (cursor != bytes.size()) { return fail<RecordFormatInfo>("trailing payload", "record.length"); }
    if (get(bytes, 36, 4) != checksum(bytes)) {
        return fail<RecordFormatInfo>("record checksum mismatch", "record.checksum");
    }
    auto id = base::DocumentId::from_integer(get(bytes, 16, 8));
    return Result<RecordFormatInfo>::success({id.value(), size.value()});
}

Result<RecordFormatInfo> write_record(std::ostream &output, const DocumentRecord &record,
                                     const RecordFormatLimits &limits) {
    auto encoded = encode_record(record, limits);
    if (!encoded) { return Result<RecordFormatInfo>::failure(std::move(encoded).error()); }
    const auto count = base::checked_narrow<std::streamsize>(encoded.value().size(), "record.length");
    if (!count) { return Result<RecordFormatInfo>::failure(count.error()); }
    if (!output.good()) { return fail<RecordFormatInfo>("output stream is not ready", "record.write"); }
    try {
        output.write(encoded.value().data(), count.value());
    } catch (const std::ios_base::failure &) {
        return fail<RecordFormatInfo>("output stream write failed", "record.write");
    }
    if (!output.good()) { return fail<RecordFormatInfo>("output stream write failed", "record.write"); }
    return Result<RecordFormatInfo>::success({record.doc_id, static_cast<std::uint64_t>(encoded.value().size())});
}
} // namespace siftwing::document
