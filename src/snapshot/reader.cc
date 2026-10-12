/*
 * PROJECT : SIFTWING
 * FILE    : reader.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : fd相对只读加载、规范解析与跨文件语义校验
 * IMPLEMENTATION : 从实际字节逐行建立临时对象；重建既有有效部件后才发布完整State。
 */
#include "siftwing/snapshot/reader.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <limits>
#include <system_error>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace siftwing::snapshot {
namespace {
using base::Result;
using View = std::string_view;
constexpr std::array<const char *, 8> names{{"documents.bin",
    "offsets.bin",
    "search-lexicon.txt",
    "search-weights.txt",
    "recommendation-dictionary.txt",
    "recommendation-characters.txt",
    "manifest.txt",
    "COMPLETE"}};
template <typename T>
Result<T> fail(const char *context) {
    return Result<T>::failure({"快照不完整、超限或与规范/部件不一致", context});
}
template <typename T>
Result<T> io(const char *context) {
    const int code = errno;
    return Result<T>::failure({std::error_code{code, std::generic_category()}.message(), context});
}
struct Fd final {
    int value;
    explicit Fd(int fd) noexcept : value(fd) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : value(std::exchange(other.value, -1)) {}
    ~Fd() {
        if (value >= 0) {
            (void)::close(value);
        }
    }
    // Linux close报错也消费fd；清空后仅关闭一次，避免重试关闭已被复用的编号。
    int close_checked() noexcept {
        const int fd = std::exchange(value, -1);
        return fd < 0 ? 0 : ::close(fd);
    }
    int release() noexcept {
        return std::exchange(value, -1);
    }
};
bool lexicon_limits(const index::LexiconLimits &v) noexcept {
    return v.max_input_terms && v.max_input_bytes && v.max_term_bytes && v.max_distinct_terms
           && v.max_vocabulary_bytes;
}
bool positive(const SnapshotReadLimits &v) noexcept {
    const auto &d = v.documents;
    const auto &s = v.search;
    const auto &r = v.recommendation;
    return d.record.max_field_bytes && d.record.max_record_bytes && d.max_documents
           && d.max_library_bytes && d.max_table_bytes && v.manifest.max_manifest_bytes
           && v.manifest.max_file_bytes && v.manifest.max_total_file_bytes
           && lexicon_limits(s.lexicon) && s.max_documents && s.max_document_tokens
           && s.max_total_tokens && s.max_postings_per_term && lexicon_limits(r.lexicon)
           && r.max_documents && r.max_total_tokens && r.max_characters && r.max_character_links;
}
std::string decimal(std::uint64_t value) {
    std::array<char, 20> out{};
    auto r = std::to_chars(out.data(), out.data() + out.size(), value);
    return {out.data(), static_cast<std::size_t>(r.ptr - out.data())};
}
Result<Fd> open_directory(const std::string &path) {
    if (path.empty() || path.find('\0') != std::string::npos) {
        return fail<Fd>("reader.path");
    }
    std::vector<std::string> parts;
    for (std::size_t start = 0; start < path.size();) {
        auto end = path.find('/', start);
        auto part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part == "..") {
            return fail<Fd>("reader.path");
        }
        if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int fd;
    do {
        fd = ::openat(AT_FDCWD, path.front() == '/' ? "/" : ".", flags, 0);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return io<Fd>("reader.open");
    }
    Fd current{fd};
    for (const auto &part : parts) {
        do {
            fd = ::openat(current.value, part.c_str(), flags, 0);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            return io<Fd>("reader.open");
        }
        Fd next{fd};
        if (current.close_checked() < 0) {
            return io<Fd>("reader.close");
        }
        current.value = next.release();
    }
    return Result<Fd>::success(std::move(current));
}
Result<void> exact_directory(int fd) {
    int copy;
    do {
        copy = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    } while (copy < 0 && errno == EINTR);
    if (copy < 0) {
        return io<void>("reader.open");
    }
    Fd owner{copy};
    DIR *raw = ::fdopendir(copy);
    if (!raw) {
        return io<void>("reader.directory");
    }
    (void)owner.release();
    // fdopendir成功接管复制fd；异常时closedir仍释放，不与原目录fd共享所有权。
    struct Directory {
        DIR *value;
        ~Directory() {
            if (value) {
                (void)::closedir(value);
            }
        }
    } directory{raw};
    std::array<bool, 8> found{};
    for (;;) {
        errno = 0;
        const auto *entry = ::readdir(raw);
        if (!entry) {
            if (errno) {
                return io<void>("reader.directory");
            }
            break;
        }
        View name{entry->d_name};
        if (name == "." || name == "..") {
            continue;
        }
        const auto it = std::find(names.begin(), names.end(), name);
        if (it == names.end()) {
            return fail<void>("reader.directory");
        }
        const auto slot = static_cast<std::size_t>(it - names.begin());
        struct stat st{};
        int r;
        do {
            r = ::fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            return io<void>("reader.stat");
        }
        if (found[slot] || !S_ISREG(st.st_mode)) {
            return fail<void>("reader.directory");
        }
        found[slot] = true;
    }
    if (!std::all_of(found.begin(), found.end(), [](bool v) {
            return v;
        })) {
        return fail<void>("reader.directory");
    }
    directory.value = nullptr;
    if (::closedir(raw) < 0) {
        return io<void>("reader.close");
    }
    return Result<void>::success();
}
bool unchanged(const struct stat &a, const struct stat &b) noexcept {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
           && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
           && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
Result<std::string> read_file(int directory,
    const char *name,
    std::uint64_t limit,
    std::optional<std::uint64_t> expected = std::nullopt) {
    int fd;
    do {
        fd = ::openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return io<std::string>("reader.open");
    }
    Fd owner{fd};
    struct stat before{}, after{};
    int r;
    do {
        r = ::fstat(fd, &before);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        return io<std::string>("reader.stat");
    }
    if (!S_ISREG(before.st_mode) || before.st_size < 0) {
        return fail<std::string>("reader.stat");
    }
    const auto size = static_cast<std::uint64_t>(before.st_size);
    if (size > limit || size > std::string{}.max_size() || (expected && size != *expected)) {
        return fail<std::string>("reader.bytes");
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto chunk = std::min<std::size_t>(bytes.size() - done, 32768);
        ssize_t got;
        do {
            got = ::pread(fd, bytes.data() + done, chunk, static_cast<off_t>(done));
        } while (got < 0 && errno == EINTR);
        if (got < 0) {
            return io<std::string>("reader.read");
        }
        if (got == 0) {
            return fail<std::string>("reader.read");
        }
        done += static_cast<std::size_t>(got);
    }
    do {
        r = ::fstat(fd, &after);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        return io<std::string>("reader.stat");
    }
    if (!unchanged(before, after)) {
        return fail<std::string>("reader.stat");
    }
    if (owner.close_checked() < 0) {
        return io<std::string>("reader.close");
    }
    return Result<std::string>::success(std::move(bytes));
}
struct Lines final {
    View rest;
    bool next(View &out) noexcept {
        const auto end = rest.find('\n');
        if (end == View::npos) {
            return false;
        }
        out = rest.substr(0, end);
        rest.remove_prefix(end + 1);
        return true;
    }
};
struct Tokens final {
    View rest;
    bool good = true;
    bool next(View &out) noexcept {
        if (rest.empty()) {
            return false;
        }
        const auto end = rest.find(' ');
        if (end == 0) {
            good = false;
            rest = {};
            return false;
        }
        out = rest.substr(0, end);
        if (end == View::npos) {
            rest = {};
        } else {
            rest.remove_prefix(end + 1);
            if (rest.empty()) {
                good = false;
            }
        }
        return true;
    }
};
template <std::size_t N>
bool split(View line, std::array<View, N> &out) noexcept {
    Tokens tokens{line};
    for (auto &field : out) {
        if (!tokens.next(field)) {
            return false;
        }
    }
    View extra;
    return !tokens.next(extra) && tokens.good;
}
bool number(View text, std::uint64_t &out) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0') || text.front() < '0'
        || text.front() > '9') {
        return false;
    }
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}
int nibble(char c) noexcept {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
bool floating(View value) noexcept {
    return value.size() == 16 && std::all_of(value.begin(), value.end(), [](char c) {
        return nibble(c) >= 0;
    });
}
Result<std::string> word(View hex, std::uint64_t limit) {
    if (hex.empty() || hex.size() % 2 || hex.size() / 2 > limit) {
        return fail<std::string>("reader.syntax");
    }
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return fail<std::string>("reader.syntax");
        }
        out.push_back(static_cast<char>(static_cast<unsigned char>((hi << 4) | lo)));
    }
    return Result<std::string>::success(std::move(out));
}
Result<std::vector<std::string>> parse_lexicon(
    View bytes, const Manifest &manifest, const SnapshotReadLimits &limits) {
    Lines lines{bytes};
    View line;
    std::array<View, 2> summary{};
    std::uint64_t count = 0;
    if (!lines.next(line) || line != "SIFTWING-LEXICON 1") {
        return fail<std::vector<std::string>>("reader.version");
    }
    if (!lines.next(line) || !split(line, summary) || summary[0] != "S"
        || !number(summary[1], count) || count != manifest.search.terms
        || count > limits.search.lexicon.max_distinct_terms || count > bytes.size() / 7) {
        return fail<std::vector<std::string>>("reader.syntax");
    }
    std::vector<std::string> terms;
    for (std::uint64_t t = 0; t < count; ++t) {
        std::array<View, 3> f{};
        std::uint64_t id = 0;
        if (!lines.next(line) || !split(line, f) || f[0] != "T" || !number(f[1], id) || id != t) {
            return fail<std::vector<std::string>>("reader.syntax");
        }
        auto key = word(f[2], limits.search.lexicon.max_term_bytes);
        if (!key) {
            return Result<std::vector<std::string>>::failure(key.error());
        }
        terms.push_back(std::move(key).value());
    }
    if (!lines.rest.empty()) {
        return fail<std::vector<std::string>>("reader.syntax");
    }
    return Result<std::vector<std::string>>::success(std::move(terms));
}
std::uint64_t network(View bytes, std::size_t position, std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[position + i]);
    }
    return value;
}
// 只复制经既有validate_record完整结构/CRC/预算验证的字段，不返回输入view；不改变旧codec API。
document::DocumentRecord record(View bytes, base::DocumentId id) {
    std::array<std::string, 6> fields;
    std::size_t cursor = document::record_header_bytes;
    for (auto &field : fields) {
        const auto size = static_cast<std::size_t>(network(bytes, cursor, 8));
        cursor += 8;
        field.assign(bytes.data() + cursor, size);
        cursor += size;
    }
    const auto kind = network(bytes, 32, 1), flags = network(bytes, 33, 1);
    document::DocumentSource source{kind == 1   ? document::SourceKind::txt
                                    : kind == 2 ? document::SourceKind::rss
                                                : document::SourceKind::jsonl,
        {std::move(fields[2]), network(bytes, 24, 8)},
        std::nullopt,
        std::nullopt,
        std::nullopt};
    if (flags & 1U) {
        source.url = std::move(fields[3]);
    }
    if (flags & 2U) {
        source.author = std::move(fields[4]);
    }
    if (flags & 4U) {
        source.published_at_raw = std::move(fields[5]);
    }
    return {id, std::move(fields[0]), std::move(fields[1]), std::move(source)};
}
Result<std::vector<document::DocumentRecord>> parse_documents(
    const std::array<std::string, 6> &bytes, const Manifest &m, const SnapshotReadLimits &limits) {
    auto table = document::decode_offset_table(bytes[1], limits.documents);
    if (!table) {
        return Result<std::vector<document::DocumentRecord>>::failure(table.error());
    }
    if (table.value().library_bytes != bytes[0].size()
        || table.value().entries.size() != m.search.documents) {
        return fail<std::vector<document::DocumentRecord>>("reader.semantic");
    }
    std::vector<document::DocumentRecord> out;
    for (const auto &entry : table.value().entries) {
        View slice{bytes[0].data() + static_cast<std::size_t>(entry.offset),
            static_cast<std::size_t>(entry.length)};
        auto valid = document::validate_record(slice, limits.documents.record);
        if (!valid) {
            return Result<std::vector<document::DocumentRecord>>::failure(valid.error());
        }
        if (valid.value().doc_id != entry.doc_id) {
            return fail<std::vector<document::DocumentRecord>>("reader.semantic");
        }
        out.push_back(record(slice, entry.doc_id));
    }
    return Result<std::vector<document::DocumentRecord>>::success(std::move(out));
}
Result<index::WeightedIndex> parse_search(View bytes,
    const std::vector<std::string> &terms,
    const std::vector<document::DocumentRecord> &records,
    const Manifest &m,
    const SnapshotReadLimits &limits) {
    Lines lines{bytes};
    View line;
    std::array<View, 4> summary{};
    std::uint64_t n = 0, total = 0, postings = 0;
    if (!lines.next(line) || line != "SIFTWING-WEIGHTS 1 1") {
        return fail<index::WeightedIndex>("reader.version");
    }
    if (!lines.next(line) || !split(line, summary) || summary[0] != "S" || !number(summary[1], n)
        || !number(summary[2], total) || !number(summary[3], postings) || n != records.size()
        || total != m.search.total_tokens || postings != m.search.postings) {
        return fail<index::WeightedIndex>("reader.semantic");
    }
    std::vector<index::IndexDocument> input;
    for (std::size_t d = 0; d < records.size(); ++d) {
        std::array<View, 5> f{};
        std::uint64_t id = 0, tokens = 0, distinct = 0;
        if (!lines.next(line) || !split(line, f) || f[0] != "D" || !number(f[1], id)
            || id != records[d].doc_id.value() || !number(f[2], tokens) || !number(f[3], distinct)
            || distinct > terms.size() || !floating(f[4])) {
            return fail<index::WeightedIndex>("reader.syntax");
        }
        input.push_back({records[d].doc_id, {{}, tokens}});
    }
    std::uint64_t seen = 0;
    for (std::size_t t = 0; t < terms.size(); ++t) {
        std::array<View, 5> f{};
        std::uint64_t id = 0, df = 0;
        if (!lines.next(line) || !split(line, f) || f[0] != "T" || !number(f[1], id) || id != t
            || !number(f[3], df) || df == 0 || df > n || df > limits.search.max_postings_per_term
            || !floating(f[4])) {
            return fail<index::WeightedIndex>("reader.syntax");
        }
        auto key = word(f[2], limits.search.lexicon.max_term_bytes);
        if (!key) {
            return Result<index::WeightedIndex>::failure(key.error());
        }
        if (key.value() != terms[t]) {
            return fail<index::WeightedIndex>("reader.semantic");
        }
        std::uint64_t previous = 0;
        for (std::uint64_t p = 0; p < df; ++p) {
            std::uint64_t term = 0, doc = 0, tf = 0;
            if (!lines.next(line) || !split(line, f) || f[0] != "P" || !number(f[1], term)
                || term != t || !number(f[2], doc) || (p && doc <= previous) || !number(f[3], tf)
                || tf == 0 || !floating(f[4]) || seen >= postings) {
                return fail<index::WeightedIndex>("reader.syntax");
            }
            const auto doc_id = base::DocumentId::from_integer(doc).value();
            const auto it = std::lower_bound(
                input.begin(), input.end(), doc_id, [](const auto &entry, auto value) {
                    return entry.document_id < value;
                });
            if (it == input.end() || it->document_id != doc_id
                || !it->frequencies.terms.emplace(terms[t], tf).second) {
                return fail<index::WeightedIndex>("reader.semantic");
            }
            previous = doc;
            ++seen;
        }
    }
    if (!lines.rest.empty() || seen != postings) {
        return fail<index::WeightedIndex>("reader.syntax");
    }
    auto raw = index::InvertedIndex::build(input, limits.search);
    if (!raw) {
        return Result<index::WeightedIndex>::failure(raw.error());
    }
    input.clear(); // raw已拥有倒排与词表，释放临时TF后再构建权重。
    auto weighted = index::WeightedIndex::build(std::move(raw).value(),
        {limits.search.max_documents,
            limits.search.lexicon.max_distinct_terms,
            limits.manifest.max_file_bytes});
    if (!weighted) {
        return weighted;
    }
    // 重新导出验证所有顺序/统计/词键/浮点位值；不悄悄接受坏权重或将已存字节改成新数值。
    auto canonical = weighted.value().serialize({limits.manifest.max_file_bytes});
    if (!canonical) {
        return Result<index::WeightedIndex>::failure(canonical.error());
    }
    if (canonical.value() != bytes) {
        return fail<index::WeightedIndex>("reader.semantic");
    }
    return weighted;
}
Result<index::RecommendationDictionary> parse_recommendation(
    View bytes, View characters, const Manifest &m, const SnapshotReadLimits &limits) {
    Lines lines{bytes};
    View line;
    std::array<View, 4> f{};
    std::uint64_t docs = 0, total = 0, count = 0;
    if (!lines.next(line) || line != "SIFTWING-RECOMMENDATION 1") {
        return fail<index::RecommendationDictionary>("reader.version");
    }
    if (!lines.next(line) || !split(line, f) || f[0] != "S" || !number(f[1], docs)
        || !number(f[2], total) || !number(f[3], count) || docs != m.recommendation.documents
        || total != m.recommendation.total_tokens || count != m.recommendation.terms
        || count > limits.recommendation.lexicon.max_distinct_terms || count > bytes.size() / 9) {
        return fail<index::RecommendationDictionary>("reader.semantic");
    }
    text::RecommendationFrequencies input{{}, total, docs};
    std::string previous;
    for (std::uint64_t t = 0; t < count; ++t) {
        std::uint64_t id = 0, frequency = 0;
        if (!lines.next(line) || !split(line, f) || f[0] != "T" || !number(f[1], id) || id != t
            || !number(f[3], frequency) || frequency == 0) {
            return fail<index::RecommendationDictionary>("reader.syntax");
        }
        auto key = word(f[2], limits.recommendation.lexicon.max_term_bytes);
        if (!key) {
            return Result<index::RecommendationDictionary>::failure(key.error());
        }
        if (t && key.value() <= previous) {
            return fail<index::RecommendationDictionary>("reader.semantic");
        }
        previous = key.value();
        input.terms.emplace(std::move(key).value(), frequency);
    }
    if (!lines.rest.empty()) {
        return fail<index::RecommendationDictionary>("reader.syntax");
    }
    auto dictionary = index::RecommendationDictionary::build(input, limits.recommendation);
    if (!dictionary) {
        return dictionary;
    }
    input.terms.clear(); // dictionary已拥有词键与频次，字符关系核验不再借用原TF。
    Lines rows{characters};
    std::array<View, 3> s{};
    std::uint64_t scalars = 0, links = 0;
    if (!rows.next(line) || line != "SIFTWING-CHARACTERS 1") {
        return fail<index::RecommendationDictionary>("reader.version");
    }
    if (!rows.next(line) || !split(line, s) || s[0] != "S" || !number(s[1], scalars)
        || !number(s[2], links) || scalars != dictionary.value().characters().size()
        || scalars != m.recommendation.characters || links != m.recommendation.character_links) {
        return fail<index::RecommendationDictionary>("reader.semantic");
    }
    std::uint64_t seen = 0;
    for (const auto &[scalar, ids] : dictionary.value().characters()) {
        if (!rows.next(line)) {
            return fail<index::RecommendationDictionary>("reader.syntax");
        }
        Tokens tokens{line};
        View token;
        std::uint64_t value = 0;
        if (!tokens.next(token) || token != "C" || !tokens.next(token) || !number(token, value)
            || value != static_cast<std::uint32_t>(scalar)) {
            return fail<index::RecommendationDictionary>("reader.semantic");
        }
        for (auto id : ids) {
            if (!tokens.next(token) || !number(token, value) || value != id.value()
                || seen >= links) {
                return fail<index::RecommendationDictionary>("reader.semantic");
            }
            ++seen;
        }
        if (tokens.next(token) || !tokens.good) {
            return fail<index::RecommendationDictionary>("reader.syntax");
        }
    }
    if (!rows.rest.empty() || seen != links) {
        return fail<index::RecommendationDictionary>("reader.semantic");
    }
    return dictionary;
}
bool metadata_budgets(const Manifest &m, const SnapshotReadLimits &limits) noexcept {
    return m.search.documents
               <= std::min(limits.documents.max_documents, limits.search.max_documents)
           && m.files[0].bytes <= limits.documents.max_library_bytes
           && m.files[1].bytes <= limits.documents.max_table_bytes
           && m.search.terms <= limits.search.lexicon.max_distinct_terms
           && m.search.total_tokens <= limits.search.max_total_tokens
           && m.recommendation.documents <= limits.recommendation.max_documents
           && m.recommendation.terms <= limits.recommendation.lexicon.max_distinct_terms
           && m.recommendation.total_tokens <= limits.recommendation.max_total_tokens
           && m.recommendation.characters <= limits.recommendation.max_characters
           && m.recommendation.character_links <= limits.recommendation.max_character_links;
}
} // namespace
struct SnapshotReader::State final {
    Manifest manifest;
    index::WeightedIndex search;
    index::RecommendationDictionary recommendation;
    std::vector<document::DocumentRecord> documents;
};
SnapshotReader::SnapshotReader(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
SnapshotReader::SnapshotReader(SnapshotReader &&other) noexcept = default;
SnapshotReader &SnapshotReader::operator=(SnapshotReader &&other) noexcept {
    if (this != &other) {
        state_ = std::move(other.state_);
    }
    return *this;
}
SnapshotReader::~SnapshotReader() = default;
const Manifest *SnapshotReader::manifest() const & noexcept {
    return state_ ? &state_->manifest : nullptr;
}
const index::WeightedIndex *SnapshotReader::search() const & noexcept {
    return state_ ? &state_->search : nullptr;
}
const index::RecommendationDictionary *SnapshotReader::recommendation() const & noexcept {
    return state_ ? &state_->recommendation : nullptr;
}
const std::vector<document::DocumentRecord> *SnapshotReader::documents() const & noexcept {
    return state_ ? &state_->documents : nullptr;
}
const document::DocumentRecord *SnapshotReader::document(base::DocumentId id) const & noexcept {
    if (!state_) {
        return nullptr;
    }
    const auto &values = state_->documents;
    const auto it =
        std::lower_bound(values.begin(), values.end(), id, [](const auto &entry, auto key) {
            return entry.doc_id < key;
        });
    return it != values.end() && it->doc_id == id ? &*it : nullptr;
}
Result<SnapshotReader> SnapshotReader::open(const std::string &parent_path,
    base::SnapshotVersion expected_version,
    const SnapshotReadLimits &limits) {
    if (!positive(limits)) {
        return fail<SnapshotReader>("reader.limits");
    }
    auto parent = open_directory(parent_path);
    if (!parent) {
        return Result<SnapshotReader>::failure(parent.error());
    }
    const auto leaf = decimal(expected_version.value());
    int fd;
    do {
        fd = ::openat(
            parent.value().value, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return io<SnapshotReader>("reader.open");
    }
    Fd directory{fd};
    auto exact = exact_directory(fd);
    if (!exact) {
        return Result<SnapshotReader>::failure(exact.error());
    }
    auto manifest_bytes = read_file(fd, "manifest.txt", limits.manifest.max_manifest_bytes);
    if (!manifest_bytes) {
        return Result<SnapshotReader>::failure(manifest_bytes.error());
    }
    auto manifest = parse_manifest(manifest_bytes.value(), limits.manifest);
    if (!manifest) {
        return Result<SnapshotReader>::failure(manifest.error());
    }
    if (manifest.value().snapshot != expected_version) {
        return fail<SnapshotReader>("reader.version");
    }
    if (!metadata_budgets(manifest.value(), limits)) {
        return fail<SnapshotReader>("reader.bytes");
    }
    auto marker = completion_marker(manifest_bytes.value(), limits.manifest);
    if (!marker) {
        return Result<SnapshotReader>::failure(marker.error());
    }
    auto complete = read_file(fd, "COMPLETE", marker.value().size(), marker.value().size());
    if (!complete) {
        return Result<SnapshotReader>::failure(complete.error());
    }
    if (complete.value() != marker.value()) {
        return fail<SnapshotReader>("manifest.completion");
    }
    std::array<std::string, 6> payloads;
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        auto bytes = read_file(
            fd, names[i], limits.manifest.max_file_bytes, manifest.value().files[i].bytes);
        if (!bytes) {
            return Result<SnapshotReader>::failure(bytes.error());
        }
        auto verified = verify_artifact(
            manifest.value(), static_cast<Artifact>(i), bytes.value(), limits.manifest);
        if (!verified) {
            return Result<SnapshotReader>::failure(verified.error());
        }
        payloads[i] = std::move(bytes).value();
    }
    auto documents = parse_documents(payloads, manifest.value(), limits);
    if (!documents) {
        return Result<SnapshotReader>::failure(documents.error());
    }
    // 解析结果拥有字段；与空串交换释放已用载荷的容量，clear只会置零长度。
    std::string{}.swap(payloads[0]);
    std::string{}.swap(payloads[1]);
    auto terms = parse_lexicon(payloads[2], manifest.value(), limits);
    if (!terms) {
        return Result<SnapshotReader>::failure(terms.error());
    }
    std::string{}.swap(payloads[2]);
    auto search =
        parse_search(payloads[3], terms.value(), documents.value(), manifest.value(), limits);
    if (!search) {
        return Result<SnapshotReader>::failure(search.error());
    }
    std::string{}.swap(payloads[3]);
    terms.value().clear();
    auto rec = parse_recommendation(payloads[4], payloads[5], manifest.value(), limits);
    if (!rec) {
        return Result<SnapshotReader>::failure(rec.error());
    }
    std::string{}.swap(payloads[4]);
    std::string{}.swap(payloads[5]);
    auto final_directory = exact_directory(fd);
    if (!final_directory) {
        return Result<SnapshotReader>::failure(final_directory.error());
    }
    if (directory.close_checked() < 0 || parent.value().close_checked() < 0) {
        return io<SnapshotReader>("reader.close");
    }
    // 全部组件/目录核验及close成功后才创建唯一拥有State；分配异常不会留下长期fd或部分Reader。
    auto state = std::unique_ptr<State>{new State{std::move(manifest).value(),
        std::move(search).value(),
        std::move(rec).value(),
        std::move(documents).value()}};
    return Result<SnapshotReader>::success(SnapshotReader{std::move(state)});
}
} // namespace siftwing::snapshot
