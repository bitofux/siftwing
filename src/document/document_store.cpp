/*
 * PROJECT : SIFTWING
 * FILE    : document_store.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 受检区间覆盖、版本化偏移表及 RAII/pread 文件读取，不缓存整个文档库
 */
#include "siftwing/document/document_store.h"
#include "siftwing/base/checked.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace siftwing::document {
namespace {
using base::Result;
constexpr std::uint64_t table_header = 32, entry_bytes = 24, min_record = 88;
template <typename T> Result<T> fail(const char *message, const char *context) {
    return Result<T>::failure({message, context});
}
template <typename T> Result<T> io_error(int code) {
    return Result<T>::failure({std::error_code{code, std::generic_category()}.message(), "store.io"});
}
bool valid_limits(const DocumentStoreLimits &v) {
    return v.record.max_field_bytes && v.record.max_record_bytes && v.max_documents &&
           v.max_library_bytes && v.max_table_bytes;
}
std::uint64_t get(std::string_view bytes, std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) { value = (value << 8U) | static_cast<unsigned char>(bytes[offset + i]); }
    return value;
}
void put(std::string &bytes, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        reinterpret_cast<unsigned char *>(bytes.data())[offset + i] =
            static_cast<unsigned char>((value >> (8U * (width - i - 1U))) & 0xffU);
    }
}
std::uint32_t table_crc(std::string_view bytes) {
    std::uint32_t value = 0xffffffffU;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i >= 24 && i < 28) { continue; }
        value ^= static_cast<unsigned char>(bytes[i]);
        for (unsigned bit = 0; bit < 8; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
        }
    }
    return value ^ 0xffffffffU;
}
Result<std::size_t> wire_size(std::uint64_t count, const DocumentStoreLimits &limits) {
    if (count > limits.max_documents) { return fail<std::size_t>("too many documents", "store.limits"); }
    auto payload = base::checked_mul(count, entry_bytes, "store.table.length");
    if (!payload) { return Result<std::size_t>::failure(payload.error()); }
    auto total = base::checked_add(payload.value(), table_header, "store.table.length");
    if (!total) { return Result<std::size_t>::failure(total.error()); }
    if (total.value() > limits.max_table_bytes) { return fail<std::size_t>("table exceeds budget", "store.limits"); }
    auto size = base::checked_narrow<std::size_t>(total.value(), "store.table.length");
    if (!size) { return size; }
    if (size.value() > std::string{}.max_size()) { return fail<std::size_t>("table exceeds capacity", "store.table.length"); }
    return size;
}
// 所有局部打开路径都先交给该 guard；只有创建完整读者之后才转交 fd。
struct Fd final {
    int value;
    explicit Fd(int input) noexcept : value(input) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : value(std::exchange(other.value, -1)) {}
    ~Fd() { if (value >= 0) { (void)::close(value); } }
    int release() noexcept { return std::exchange(value, -1); }
};
Result<Fd> open_file(const std::string &path) {
    if (path.empty() || path.find('\0') != std::string::npos) { return fail<Fd>("invalid library path", "store.path"); }
    int fd;
    do { fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK); } while (fd < 0 && errno == EINTR);
    if (fd < 0) { return io_error<Fd>(errno); }
    return Result<Fd>::success(Fd{fd});
}
Result<std::uint64_t> file_size(int fd, const DocumentStoreLimits &limits) {
    struct stat info{};
    if (::fstat(fd, &info) < 0) { return io_error<std::uint64_t>(errno); }
    if (!S_ISREG(info.st_mode)) { return fail<std::uint64_t>("library must be a regular file", "store.file"); }
    auto size = base::checked_narrow<std::uint64_t>(info.st_size, "store.length");
    if (!size) { return size; }
    if (size.value() > limits.max_library_bytes) { return fail<std::uint64_t>("library exceeds budget", "store.limits"); }
    return size;
}
Result<void> same_size(int fd, std::uint64_t expected, const DocumentStoreLimits &limits) {
    auto size = file_size(fd, limits);
    if (!size) { return Result<void>::failure(size.error()); }
    if (size.value() != expected) { return fail<void>("library size changed or differs from table", "store.length"); }
    return Result<void>::success();
}
Result<std::string> read_bytes(int fd, std::uint64_t offset, std::uint64_t length,
                               std::uint64_t library_bytes) {
    if (offset > library_bytes || length > library_bytes - offset) {
        return fail<std::string>("record outside library", "store.table.range");
    }
    auto position = base::checked_narrow<off_t>(offset, "store.length");
    auto size = base::checked_narrow<std::size_t>(length, "store.length");
    if (!position) { return Result<std::string>::failure(position.error()); }
    if (!size) { return Result<std::string>::failure(size.error()); }
    if (size.value() > std::string{}.max_size()) { return fail<std::string>("record exceeds capacity", "store.length"); }
    std::string bytes(size.value(), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        // fstat 给出的稳定大小可由 off_t 表示；仍在每次 pread 前检查数值转换。
        auto current = base::checked_narrow<off_t>(offset + static_cast<std::uint64_t>(done), "store.length");
        if (!current) { return Result<std::string>::failure(current.error()); }
        const auto chunk = std::min(bytes.size() - done,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto count = ::pread(fd, bytes.data() + done, chunk, current.value());
        if (count < 0) {
            if (errno == EINTR) { continue; }
            return io_error<std::string>(errno);
        }
        if (count == 0) { return fail<std::string>("unexpected EOF while reading record", "store.length"); }
        done += static_cast<std::size_t>(count);
    }
    return Result<std::string>::success(std::move(bytes));
}
Result<std::string> checked_record(int fd, const OffsetEntry &entry, std::uint64_t library_bytes,
                                  const DocumentStoreLimits &limits) {
    auto stable = same_size(fd, library_bytes, limits);
    if (!stable) { return Result<std::string>::failure(stable.error()); }
    auto bytes = read_bytes(fd, entry.offset, entry.length, library_bytes);
    if (!bytes) { return bytes; }
    auto valid = validate_record(bytes.value(), limits.record);
    if (!valid) { return Result<std::string>::failure(valid.error()); }
    if (valid.value().doc_id != entry.doc_id) { return fail<std::string>("record identity differs from table", "store.identity"); }
    auto verified_again = same_size(fd, library_bytes, limits);
    if (!verified_again) { return Result<std::string>::failure(verified_again.error()); }
    return bytes;
}
// 只消费已经通过完整结构/CRC/预算校验的字节，复制各字段而不返回悬空 view。
DocumentRecord decode_record(std::string_view bytes, base::DocumentId id) {
    std::array<std::string, 6> fields;
    std::size_t cursor = record_header_bytes;
    for (auto &field : fields) {
        const auto length = static_cast<std::size_t>(get(bytes, cursor, 8));
        cursor += 8;
        field.assign(bytes.data() + cursor, length);
        cursor += length;
    }
    const auto kind = get(bytes, 32, 1), flags = get(bytes, 33, 1);
    DocumentSource source{kind == 1 ? SourceKind::txt : kind == 2 ? SourceKind::rss : SourceKind::jsonl,
                          {std::move(fields[2]), get(bytes, 24, 8)}, std::nullopt, std::nullopt, std::nullopt};
    if (flags & 1U) { source.url = std::move(fields[3]); }
    if (flags & 2U) { source.author = std::move(fields[4]); }
    if (flags & 4U) { source.published_at_raw = std::move(fields[5]); }
    return {id, std::move(fields[0]), std::move(fields[1]), std::move(source)};
}
} // namespace

Result<void> validate_offset_table(const OffsetTable &table, const DocumentStoreLimits &limits) {
    if (!valid_limits(limits)) { return fail<void>("budgets must be positive", "store.limits"); }
    auto count = base::checked_narrow<std::uint64_t>(table.entries.size(), "store.table.length");
    if (!count) { return Result<void>::failure(count.error()); }
    auto size = wire_size(count.value(), limits);
    if (!size) { return Result<void>::failure(size.error()); }
    if (table.library_bytes > limits.max_library_bytes) { return fail<void>("library exceeds budget", "store.limits"); }
    std::vector<const OffsetEntry *> physical;
    physical.reserve(table.entries.size());
    for (std::size_t i = 0; i < table.entries.size(); ++i) {
        const auto &entry = table.entries[i];
        if (i > 0 && !(table.entries[i - 1].doc_id < entry.doc_id)) {
            return fail<void>("identities must be strictly increasing", "store.table.order");
        }
        if (entry.length < min_record || entry.length > limits.record.max_record_bytes ||
            entry.offset > table.library_bytes || entry.length > table.library_bytes - entry.offset) {
            return fail<void>("record interval outside library or budget", "store.table.range");
        }
        physical.push_back(&entry);
    }
    std::sort(physical.begin(), physical.end(), [](const auto *a, const auto *b) { return a->offset < b->offset; });
    std::uint64_t cursor = 0;
    for (const auto *entry : physical) {
        if (entry->offset != cursor) { return fail<void>("library has holes or overlapping records", "store.table.range"); }
        // 每个区间已由 library_bytes-offset 证明不会回绕。
        cursor += entry->length;
    }
    if (cursor != table.library_bytes) { return fail<void>("table does not cover complete library", "store.table.range"); }
    return Result<void>::success();
}

Result<std::string> encode_offset_table(const OffsetTable &table, const DocumentStoreLimits &limits) {
    auto valid = validate_offset_table(table, limits);
    if (!valid) { return Result<std::string>::failure(valid.error()); }
    const auto count = static_cast<std::uint64_t>(table.entries.size());
    const auto size = wire_size(count, limits).value();
    std::string bytes(size, '\0');
    bytes.replace(0, 4, "SWDI");
    put(bytes, 4, offset_table_version, 2); put(bytes, 6, table_header, 2);
    put(bytes, 8, count, 8); put(bytes, 16, table.library_bytes, 8);
    std::size_t cursor = static_cast<std::size_t>(table_header);
    for (const auto &entry : table.entries) {
        put(bytes, cursor, entry.doc_id.value(), 8); put(bytes, cursor + 8, entry.offset, 8);
        put(bytes, cursor + 16, entry.length, 8); cursor += static_cast<std::size_t>(entry_bytes);
    }
    put(bytes, 24, table_crc(bytes), 4);
    return Result<std::string>::success(std::move(bytes));
}

Result<OffsetTable> decode_offset_table(std::string_view bytes, const DocumentStoreLimits &limits) {
    if (!valid_limits(limits)) { return fail<OffsetTable>("budgets must be positive", "store.limits"); }
    if (bytes.size() < table_header || bytes.size() > limits.max_table_bytes) {
        return fail<OffsetTable>("table size outside limits", "store.table.length");
    }
    if (bytes.substr(0, 4) != "SWDI" || get(bytes, 4, 2) != offset_table_version ||
        get(bytes, 6, 2) != table_header || get(bytes, 28, 4) != 0) {
        return fail<OffsetTable>("unsupported table header", "store.table.header");
    }
    auto size = wire_size(get(bytes, 8, 8), limits);
    if (!size) { return Result<OffsetTable>::failure(size.error()); }
    if (size.value() != bytes.size()) { return fail<OffsetTable>("table count/length mismatch", "store.table.length"); }
    if (get(bytes, 24, 4) != table_crc(bytes)) { return fail<OffsetTable>("table checksum mismatch", "store.table.checksum"); }
    OffsetTable table{{}, get(bytes, 16, 8)};
    if (table.library_bytes > limits.max_library_bytes) { return fail<OffsetTable>("library exceeds budget", "store.limits"); }
    table.entries.reserve((bytes.size() - static_cast<std::size_t>(table_header)) / static_cast<std::size_t>(entry_bytes));
    for (std::size_t cursor = static_cast<std::size_t>(table_header); cursor < bytes.size(); cursor += static_cast<std::size_t>(entry_bytes)) {
        auto id = base::DocumentId::from_integer(get(bytes, cursor, 8));
        table.entries.push_back({id.value(), get(bytes, cursor + 8, 8), get(bytes, cursor + 16, 8)});
    }
    auto valid = validate_offset_table(table, limits);
    if (!valid) { return Result<OffsetTable>::failure(valid.error()); }
    return Result<OffsetTable>::success(std::move(table));
}

Result<OffsetTable> build_offset_table(const std::string &path, const DocumentStoreLimits &limits) {
    if (!valid_limits(limits)) { return fail<OffsetTable>("budgets must be positive", "store.limits"); }
    auto file = open_file(path);
    if (!file) { return Result<OffsetTable>::failure(file.error()); }
    auto size = file_size(file.value().value, limits);
    if (!size) { return Result<OffsetTable>::failure(size.error()); }
    OffsetTable table{{}, size.value()};
    std::uint64_t offset = 0;
    while (offset < table.library_bytes) {
        // 新表项的持久化预算也先验，避免扫描完成才发现表空间不够。
        auto next = base::checked_add(static_cast<std::uint64_t>(table.entries.size()), std::uint64_t{1}, "store.table.length");
        if (!next) { return Result<OffsetTable>::failure(next.error()); }
        auto capacity = wire_size(next.value(), limits);
        if (!capacity) { return Result<OffsetTable>::failure(capacity.error()); }
        auto header = read_bytes(file.value().value, offset, record_header_bytes, table.library_bytes);
        if (!header) { return Result<OffsetTable>::failure(header.error()); }
        const auto length = get(header.value(), 8, 8);
        if (length < min_record || length > limits.record.max_record_bytes) {
            return fail<OffsetTable>("declared record size outside budget", "store.length");
        }
        auto bytes = read_bytes(file.value().value, offset, length, table.library_bytes);
        if (!bytes) { return Result<OffsetTable>::failure(bytes.error()); }
        auto valid = validate_record(bytes.value(), limits.record);
        if (!valid) { return Result<OffsetTable>::failure(valid.error()); }
        table.entries.push_back({valid.value().doc_id, offset, length});
        offset += length;
    }
    auto stable = same_size(file.value().value, table.library_bytes, limits);
    if (!stable) { return Result<OffsetTable>::failure(stable.error()); }
    std::sort(table.entries.begin(), table.entries.end(), [](const auto &a, const auto &b) { return a.doc_id < b.doc_id; });
    auto valid = validate_offset_table(table, limits);
    if (!valid) { return Result<OffsetTable>::failure(valid.error()); }
    return Result<OffsetTable>::success(std::move(table));
}

DocumentStoreReader::DocumentStoreReader(int fd, OffsetTable table, DocumentStoreLimits limits) noexcept
    : fd_(fd), table_(std::move(table)), limits_(limits) {}
DocumentStoreReader::DocumentStoreReader(DocumentStoreReader &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)), table_(std::move(other.table_)), limits_(other.limits_) {
    other.table_.entries.clear(); other.table_.library_bytes = 0;
}
DocumentStoreReader &DocumentStoreReader::operator=(DocumentStoreReader &&other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) { (void)::close(fd_); }
        fd_ = std::exchange(other.fd_, -1); table_ = std::move(other.table_); limits_ = other.limits_;
        other.table_.entries.clear(); other.table_.library_bytes = 0;
    }
    return *this;
}
DocumentStoreReader::~DocumentStoreReader() { if (fd_ >= 0) { (void)::close(fd_); } }
Result<DocumentStoreReader> DocumentStoreReader::open(const std::string &path, const OffsetTable &table,
                                                     const DocumentStoreLimits &limits) {
    auto valid = validate_offset_table(table, limits);
    if (!valid) { return Result<DocumentStoreReader>::failure(valid.error()); }
    auto file = open_file(path);
    if (!file) { return Result<DocumentStoreReader>::failure(file.error()); }
    auto stable = same_size(file.value().value, table.library_bytes, limits);
    if (!stable) { return Result<DocumentStoreReader>::failure(stable.error()); }
    for (const auto &entry : table.entries) {
        auto record = checked_record(file.value().value, entry, table.library_bytes, limits);
        if (!record) { return Result<DocumentStoreReader>::failure(record.error()); }
    }
    OffsetTable owned = table;
    return Result<DocumentStoreReader>::success(DocumentStoreReader{file.value().release(), std::move(owned), limits});
}
Result<DocumentRecord> DocumentStoreReader::read(base::DocumentId doc_id) const {
    if (fd_ < 0) { return fail<DocumentRecord>("reader was moved from", "store.closed"); }
    const auto entry = std::lower_bound(table_.entries.begin(), table_.entries.end(), doc_id,
        [](const auto &candidate, auto id) { return candidate.doc_id < id; });
    if (entry == table_.entries.end() || entry->doc_id != doc_id) { return fail<DocumentRecord>("document not found", "store.not_found"); }
    auto bytes = checked_record(fd_, *entry, table_.library_bytes, limits_);
    if (!bytes) { return Result<DocumentRecord>::failure(bytes.error()); }
    return Result<DocumentRecord>::success(decode_record(bytes.value(), doc_id));
}
} // namespace siftwing::document
