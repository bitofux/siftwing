/*
 * PROJECT : SIFTWING
 * FILE    : writer.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 固定payload编码、fd相对写入/回读、同步及无覆盖原子发布
 * IMPLEMENTATION : 全部可能分配的成功结果在rename提交前建立；失败临时目录保留。
 */
#include "siftwing/snapshot/writer.h"
#include "siftwing/base/checked.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace siftwing::snapshot {
namespace {
using base::Result;
constexpr char hex_digits[] = "0123456789abcdef";
constexpr std::array<const char *,8> file_names{{"documents.bin", "offsets.bin",
    "search-lexicon.txt", "search-weights.txt", "recommendation-dictionary.txt",
    "recommendation-characters.txt", "manifest.txt", "COMPLETE"}};
template <typename T> Result<T> fail(const char *message, const char *context) {
    return Result<T>::failure({message, context});
}
template <typename T> Result<T> io_error(int code, const char *context, std::string_view temp = {}) {
    auto message = std::error_code{code,std::generic_category()}.message();
    if (!temp.empty()) { message += "; 保留临时目录: "; message.append(temp); }
    return Result<T>::failure({std::move(message), context});
}
struct Fd final {
    int value;
    explicit Fd(int fd) noexcept : value(fd) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : value(std::exchange(other.value,-1)) {}
    ~Fd() { if (value >= 0) { (void)::close(value); } }
    // Linux close即使报错也消费句柄；先清空成员，绝不重试可能已被复用的fd。
    int close_checked() noexcept {
        const int fd = std::exchange(value,-1);
        return fd < 0 ? 0 : ::close(fd);
    }
    int release() noexcept { return std::exchange(value,-1); }
};
bool positive_limits(const SnapshotWriteLimits &v) noexcept {
    const auto &d = v.documents;
    return d.record.max_field_bytes && d.record.max_record_bytes && d.max_documents &&
        d.max_library_bytes && d.max_table_bytes && v.manifest.max_manifest_bytes &&
        v.manifest.max_file_bytes && v.manifest.max_total_file_bytes;
}
Result<std::vector<std::string>> path_components(const std::string &path) {
    if (path.empty() || path.find('\0') != std::string::npos) {
        return fail<std::vector<std::string>>("输出父路径为空或含NUL", "writer.path");
    }
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/',start);
        const auto part = path.substr(start,end == std::string::npos ? end : end-start);
        if (part == "..") { return fail<std::vector<std::string>>("输出路径不能含..组件", "writer.path"); }
        if (!part.empty() && part != ".") { components.push_back(part); }
        if (end == std::string::npos) { break; }
        start = end+1;
    }
    return Result<std::vector<std::string>>::success(std::move(components));
}
Result<Fd> open_parent(bool absolute, const std::vector<std::string> &components) {
    constexpr int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    int fd;
    do { fd = ::openat(AT_FDCWD,absolute ? "/" : ".",flags,0); } while (fd < 0 && errno == EINTR);
    if (fd < 0) { return io_error<Fd>(errno,"writer.open"); }
    Fd current{fd};
    for (const auto &component : components) {
        do { fd = ::openat(current.value,component.c_str(),flags,0); } while (fd < 0 && errno == EINTR);
        if (fd < 0) { return io_error<Fd>(errno,"writer.open"); }
        Fd next{fd};
        if (current.close_checked() < 0) { return io_error<Fd>(errno,"writer.close"); }
        current.value = next.release();
    }
    return Result<Fd>::success(std::move(current));
}
std::string decimal(std::uint64_t value) {
    std::array<char,20> bytes{};
    const auto result = std::to_chars(bytes.data(),bytes.data()+bytes.size(),value);
    return {bytes.data(),static_cast<std::size_t>(result.ptr-bytes.data())};
}
// 每个追加先检查剩余字节，包含头、空格、hex膨胀及LF；失败不交付部分文本。
struct Text final {
    std::string bytes;
    std::uint64_t limit;
    bool append(std::string_view value) {
        if (value.size() > limit-static_cast<std::uint64_t>(bytes.size())) { return false; }
        bytes.append(value); return true;
    }
    bool number(std::uint64_t value) {
        std::array<char,20> digits{};
        const auto result = std::to_chars(digits.data(),digits.data()+digits.size(),value);
        return append({digits.data(),static_cast<std::size_t>(result.ptr-digits.data())});
    }
    bool hex(std::string_view word) {
        if (word.size() > (limit-static_cast<std::uint64_t>(bytes.size()))/2) { return false; }
        for (unsigned char c : word) {
            bytes.push_back(hex_digits[c>>4U]); bytes.push_back(hex_digits[c&15U]);
        }
        return true;
    }
};
Result<std::string> encode_lexicon(const index::Lexicon &lexicon, std::uint64_t limit) {
    Text text{{},limit}; const auto &terms = lexicon.terms();
    if (!text.append("SIFTWING-LEXICON 1\nS ") || !text.number(terms.size()) || !text.append("\n")) {
        return fail<std::string>("词表编码超过字节预算", "writer.bytes");
    }
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (!text.append("T ") || !text.number(i) || !text.append(" ") || !text.hex(terms[i]) || !text.append("\n")) {
            return fail<std::string>("词表编码超过字节预算", "writer.bytes");
        }
    }
    return Result<std::string>::success(std::move(text.bytes));
}
Result<std::string> encode_dictionary(const index::RecommendationDictionary &dictionary, std::uint64_t limit) {
    Text text{{},limit}; const auto &terms = dictionary.lexicon().terms();
    if (!text.append("SIFTWING-RECOMMENDATION 1\nS ") || !text.number(dictionary.documents()) ||
        !text.append(" ") || !text.number(dictionary.total_tokens()) || !text.append(" ") ||
        !text.number(terms.size()) || !text.append("\n")) {
        return fail<std::string>("推荐编码超过字节预算", "writer.bytes");
    }
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (!text.append("T ") || !text.number(i) || !text.append(" ") || !text.hex(terms[i]) ||
            !text.append(" ") || !text.number(dictionary.frequencies()[i]) || !text.append("\n")) {
            return fail<std::string>("推荐编码超过字节预算", "writer.bytes");
        }
    }
    return Result<std::string>::success(std::move(text.bytes));
}
Result<std::string> encode_characters(const index::RecommendationDictionary &dictionary,
                                     std::uint64_t links, std::uint64_t limit) {
    Text text{{},limit};
    if (!text.append("SIFTWING-CHARACTERS 1\nS ") || !text.number(dictionary.characters().size()) ||
        !text.append(" ") || !text.number(links) || !text.append("\n")) {
        return fail<std::string>("字符索引编码超过字节预算", "writer.bytes");
    }
    for (const auto &[scalar,ids] : dictionary.characters()) {
        if (!text.append("C ") || !text.number(static_cast<std::uint32_t>(scalar))) {
            return fail<std::string>("字符索引编码超过字节预算", "writer.bytes");
        }
        for (const auto id : ids) {
            if (!text.append(" ") || !text.number(id.value())) {
                return fail<std::string>("字符索引编码超过字节预算", "writer.bytes");
            }
        }
        if (!text.append("\n")) { return fail<std::string>("字符索引编码超过字节预算", "writer.bytes"); }
    }
    return Result<std::string>::success(std::move(text.bytes));
}
struct Bundle final {
    std::array<std::string,8> bytes;
    Manifest manifest;
};
Result<Bundle> encode(const std::vector<document::DocumentRecord> &records,
                      const index::WeightedIndex &search, const index::RecommendationDictionary &rec,
                      const SnapshotMetadata &metadata, const SnapshotWriteLimits &limits) {
    const auto &raw = search.raw_index(); const auto stats = raw.statistics();
    if (records.size() > limits.documents.max_documents || records.size() != raw.documents().size()) {
        return fail<Bundle>("记录数超限或与搜索集合不同", "writer.documents");
    }
    std::vector<const document::DocumentRecord *> ordered;
    ordered.reserve(records.size());
    for (const auto &record : records) { ordered.push_back(&record); }
    std::sort(ordered.begin(),ordered.end(),[](const auto *a, const auto *b) { return a->doc_id < b->doc_id; });
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (ordered[i]->doc_id != raw.documents()[i].document_id) {
            return fail<Bundle>("记录身份集合与搜索集合不同或重复", "writer.identity");
        }
    }
    std::uint64_t links = 0;
    for (const auto &[scalar,ids] : rec.characters()) {
        (void)scalar;
        auto sum = base::checked_add(links,static_cast<std::uint64_t>(ids.size()),"writer.bytes");
        if (!sum) { return Result<Bundle>::failure(sum.error()); }
        links = sum.value();
    }
    Bundle bundle{{},Manifest{metadata.snapshot,metadata.build_version,
        {stats.documents,static_cast<std::uint64_t>(raw.lexicon().terms().size()),stats.total_tokens,stats.total_postings},
        metadata.search_provenance,{rec.documents(),static_cast<std::uint64_t>(rec.lexicon().terms().size()),
            rec.total_tokens(),static_cast<std::uint64_t>(rec.characters().size()),links},metadata.recommendation_provenance,{}}};
    auto &payloads = bundle.bytes;
    const auto library_limit = std::min({limits.documents.max_library_bytes,limits.manifest.max_file_bytes,
        limits.manifest.max_total_file_bytes});
    document::OffsetTable offsets{{},0};
    for (const auto *record : ordered) {
        const auto remaining = library_limit-static_cast<std::uint64_t>(payloads[0].size());
        if (remaining < 88) { return fail<Bundle>("文档库超过字节预算", "writer.bytes"); }
        auto bytes = document::encode_record(*record,{limits.documents.record.max_field_bytes,
            std::min(limits.documents.record.max_record_bytes,remaining)});
        if (!bytes) { return Result<Bundle>::failure(bytes.error()); }
        offsets.entries.push_back({record->doc_id,static_cast<std::uint64_t>(payloads[0].size()),
            static_cast<std::uint64_t>(bytes.value().size())});
        payloads[0] += bytes.value();
    }
    offsets.library_bytes = payloads[0].size();
    std::uint64_t total = offsets.library_bytes;
    // 为每一输出立即限制总预算的剩余额度，不先分配整个超限payload再核总和。
    for (std::size_t role = 1; role < 6; ++role) {
        const auto available = std::min(limits.manifest.max_file_bytes,limits.manifest.max_total_file_bytes-total);
        if (available == 0) { return fail<Bundle>("完整快照超过总字节预算", "writer.bytes"); }
        const auto produce = [&]() -> Result<std::string> {
            if (role == 1) {
                auto table_limits = limits.documents;
                table_limits.max_table_bytes = std::min(table_limits.max_table_bytes,available);
                return document::encode_offset_table(offsets,table_limits);
            }
            if (role == 2) { return encode_lexicon(raw.lexicon(),available); }
            if (role == 3) { return search.serialize({available}); }
            if (role == 4) { return encode_dictionary(rec,available); }
            return encode_characters(rec,links,available);
        };
        auto bytes = produce();
        if (!bytes) { return Result<Bundle>::failure(bytes.error()); }
        total += static_cast<std::uint64_t>(bytes.value().size()); // 已由剩余预算限制，不会溢出。
        payloads[role] = std::move(bytes).value();
    }
    for (std::size_t i = 0; i < 6; ++i) {
        auto hash = sha256_bytes(payloads[i]);
        if (!hash) { return Result<Bundle>::failure(hash.error()); }
        bundle.manifest.files[i] = {artifact_layout[i].version,static_cast<std::uint64_t>(payloads[i].size()),
            std::move(hash).value()};
    }
    auto manifest = serialize_manifest(bundle.manifest,limits.manifest);
    if (!manifest) { return Result<Bundle>::failure(manifest.error()); }
    payloads[6] = std::move(manifest).value();
    auto marker = completion_marker(payloads[6],limits.manifest);
    if (!marker) { return Result<Bundle>::failure(marker.error()); }
    payloads[7] = std::move(marker).value();
    return Result<Bundle>::success(std::move(bundle));
}
int sync_fd(int fd) noexcept {
    int code;
    do { code = ::fsync(fd); } while (code < 0 && errno == EINTR);
    return code;
}
Result<std::string> create_temp(int parent, const std::string &version) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::array<unsigned char,16> random{};
        std::size_t done = 0;
        while (done < random.size()) {
            const auto count = ::getrandom(random.data()+done,random.size()-done,0);
            if (count < 0) { if (errno == EINTR) { continue; } return io_error<std::string>(errno,"writer.random"); }
            if (count == 0) { return fail<std::string>("随机源提前结束", "writer.random"); }
            done += static_cast<std::size_t>(count);
        }
        auto name = ".siftwing-"+version+"-";
        for (auto byte : random) { name += hex_digits[byte>>4U]; name += hex_digits[byte&15U]; }
        name += ".tmp";
        int code;
        do { code = ::mkdirat(parent,name.c_str(),0700); } while (code < 0 && errno == EINTR);
        if (code == 0) { return Result<std::string>::success(std::move(name)); }
        if (errno != EEXIST) { return io_error<std::string>(errno,"writer.mkdir"); }
    }
    return fail<std::string>("随机临时目录连续冲突，未修改既有目录", "writer.mkdir");
}
Result<void> write_verified_file(int directory, const char *name, std::string_view bytes,
                                 std::string_view temp) {
    int fd;
    do { fd = ::openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600); }
    while (fd < 0 && errno == EINTR);
    if (fd < 0) { return io_error<void>(errno,"writer.open",temp); }
    Fd output{fd};
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto chunk = std::min(bytes.size()-done,static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto count = ::write(output.value,bytes.data()+done,chunk);
        if (count < 0) { if (errno == EINTR) { continue; } return io_error<void>(errno,"writer.write",temp); }
        if (count == 0) { return io_error<void>(EIO,"writer.write",temp); }
        done += static_cast<std::size_t>(count);
    }
    if (sync_fd(output.value) < 0) { return io_error<void>(errno,"writer.sync",temp); }
    if (output.close_checked() < 0) { return io_error<void>(errno,"writer.close",temp); }
    do { fd = ::openat(directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0); }
    while (fd < 0 && errno == EINTR);
    if (fd < 0) { return io_error<void>(errno,"writer.open",temp); }
    Fd input{fd};
    const auto size_matches = [&]() -> Result<void> {
        struct stat info{};
        int code;
        do { code = ::fstat(input.value,&info); } while (code < 0 && errno == EINTR);
        if (code < 0) { return io_error<void>(errno,"writer.stat",temp); }
        if (!S_ISREG(info.st_mode) || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) != bytes.size()) {
            return io_error<void>(EIO,"writer.stat",temp);
        }
        return Result<void>::success();
    };
    auto sized = size_matches(); if (!sized) { return sized; }
    std::array<char,16384> buffer{}; done = 0;
    while (done < bytes.size()) {
        const auto position = base::checked_narrow<off_t>(done,"writer.read");
        if (!position) { return Result<void>::failure({position.error().message+"; 保留临时目录: "+std::string{temp},"writer.read"}); }
        const auto count = ::pread(input.value,buffer.data(),std::min(buffer.size(),bytes.size()-done),position.value());
        if (count < 0) { if (errno == EINTR) { continue; } return io_error<void>(errno,"writer.read",temp); }
        if (count == 0) { return io_error<void>(EIO,"writer.read",temp); }
        const auto amount = static_cast<std::size_t>(count);
        // 与已经参与manifest摘要的原字节逐字节一致，比仅比较摘要更严格。
        if (std::memcmp(buffer.data(),bytes.data()+done,amount) != 0) { return io_error<void>(EIO,"writer.read",temp); }
        done += amount;
    }
    auto stable = size_matches(); if (!stable) { return stable; }
    if (input.close_checked() < 0) { return io_error<void>(errno,"writer.close",temp); }
    return Result<void>::success();
}
Result<void> exact_directory(int directory, std::string_view temp) {
    int duplicate;
    do { duplicate = ::fcntl(directory,F_DUPFD_CLOEXEC,0); } while (duplicate < 0 && errno == EINTR);
    if (duplicate < 0) { return io_error<void>(errno,"writer.directory",temp); }
    Fd duplicated{duplicate};
    DIR *raw = ::fdopendir(duplicate);
    if (!raw) { return io_error<void>(errno,"writer.directory",temp); }
    (void)duplicated.release();
    struct Directory final {
        DIR *value;
        ~Directory() { if (value) { (void)::closedir(value); } }
    } stream{raw};
    std::array<bool,8> found{};
    for (;;) {
        errno = 0;
        const auto *entry = ::readdir(stream.value);
        if (!entry) { if (errno) { return io_error<void>(errno,"writer.directory",temp); } break; }
        if (std::strcmp(entry->d_name,".") == 0 || std::strcmp(entry->d_name,"..") == 0) { continue; }
        const auto name = std::find_if(file_names.begin(),file_names.end(),[&](const auto *expected) {
            return std::strcmp(entry->d_name,expected) == 0;
        });
        if (name == file_names.end()) { return io_error<void>(EINVAL,"writer.directory",temp); }
        const auto slot = static_cast<std::size_t>(name-file_names.begin());
        struct stat info{}; int code;
        do { code = ::fstatat(directory,entry->d_name,&info,AT_SYMLINK_NOFOLLOW); } while (code < 0 && errno == EINTR);
        if (code < 0) { return io_error<void>(errno,"writer.directory",temp); }
        if (found[slot] || !S_ISREG(info.st_mode)) { return io_error<void>(EINVAL,"writer.directory",temp); }
        found[slot] = true;
    }
    if (!std::all_of(found.begin(),found.end(),[](bool value) { return value; })) {
        return io_error<void>(EINVAL,"writer.directory",temp);
    }
    DIR *closing = std::exchange(stream.value,nullptr);
    if (::closedir(closing) < 0) { return io_error<void>(errno,"writer.directory",temp); }
    return Result<void>::success();
}
} // namespace

Result<PublishedSnapshot> write_snapshot(const std::string &parent_path,
    const std::vector<document::DocumentRecord> &documents, const index::WeightedIndex &search,
    const index::RecommendationDictionary &recommendation, const SnapshotMetadata &metadata,
    const SnapshotWriteLimits &limits) {
    static_assert(document::record_format_version == artifact_layout[0].version);
    static_assert(document::offset_table_version == artifact_layout[1].version);
    static_assert(index::weight_serialization_version == artifact_layout[3].version);
    static_assert(index::weight_policy_version == 1);
    static_assert(std::is_nothrow_move_constructible_v<Result<PublishedSnapshot>>);
    if (!positive_limits(limits)) { return fail<PublishedSnapshot>("全部预算必须为正", "writer.limits"); }
    auto components = path_components(parent_path);
    if (!components) { return Result<PublishedSnapshot>::failure(components.error()); }
    auto bundle = encode(documents,search,recommendation,metadata,limits);
    if (!bundle) { return Result<PublishedSnapshot>::failure(bundle.error()); }
    const auto version = decimal(metadata.snapshot.value());
    auto opened_parent = open_parent(parent_path.front() == '/',components.value());
    if (!opened_parent) { return Result<PublishedSnapshot>::failure(opened_parent.error()); }
    Fd parent = std::move(opened_parent).value();
    struct stat existing{}; int code;
    do { code = ::fstatat(parent.value,version.c_str(),&existing,AT_SYMLINK_NOFOLLOW); } while (code < 0 && errno == EINTR);
    if (code == 0) { return fail<PublishedSnapshot>("正式版本已存在，不覆盖任何对象", "writer.exists"); }
    if (errno != ENOENT) { return io_error<PublishedSnapshot>(errno,"writer.stat"); }
    auto created = create_temp(parent.value,version);
    if (!created) { return Result<PublishedSnapshot>::failure(created.error()); }
    const auto &temp = created.value();
    int fd;
    do { fd = ::openat(parent.value,temp.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0); }
    while (fd < 0 && errno == EINTR);
    if (fd < 0) { return io_error<PublishedSnapshot>(errno,"writer.open",temp); }
    Fd directory{fd};
    for (std::size_t i = 0; i < file_names.size(); ++i) {
        auto saved = write_verified_file(directory.value,file_names[i],bundle.value().bytes[i],temp);
        if (!saved) { return Result<PublishedSnapshot>::failure(saved.error()); }
    }
    auto exact = exact_directory(directory.value,temp);
    if (!exact) { return Result<PublishedSnapshot>::failure(exact.error()); }
    if (sync_fd(directory.value) < 0) { return io_error<PublishedSnapshot>(errno,"writer.sync",temp); }
    if (directory.close_checked() < 0) { return io_error<PublishedSnapshot>(errno,"writer.close",temp); }
    // 从这里起成功路径不再分配。rename是唯一提交点；后续错误不得伪报为未发布。
    auto published = Result<PublishedSnapshot>::success(
        PublishedSnapshot{std::move(bundle.value().manifest),PublicationDurability::synced,0});
    if (::renameat2(parent.value,temp.c_str(),parent.value,version.c_str(),RENAME_NOREPLACE) < 0) {
        return io_error<PublishedSnapshot>(errno,"writer.rename",temp);
    }
    if (sync_fd(parent.value) < 0) {
        published.value().durability = PublicationDurability::unconfirmed;
        published.value().system_errno = errno;
    }
    if (parent.close_checked() < 0 && published.value().system_errno == 0) {
        published.value().durability = PublicationDurability::unconfirmed;
        published.value().system_errno = errno;
    }
    return published;
}
} // namespace siftwing::snapshot
