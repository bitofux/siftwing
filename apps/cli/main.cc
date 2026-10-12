/*
 * PROJECT : SIFTWING
 * FILE    : main.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 从完整只读快照执行独立SEARCH/SUGGEST并输出有界JSON
 * IMPLEMENTATION : 本次拥有Reader与管线，原DocId回查；先完整准入和显示，再写报告。
 */
#include "siftwing/snapshot/reader.hpp"
#include "siftwing/query/top_k.hpp"
#include "siftwing/query/recommendation.hpp"
#include "siftwing/query/summary.hpp"
#include "siftwing/query/highlight.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include "siftwing/text/cppjieba_tokenizer.hpp"
#include <utf8.h>
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace siftwing;
constexpr std::uint64_t mib = 1024U * 1024U;
constexpr std::uint64_t work_cap = 50000000;
struct Failure final : std::runtime_error {
    std::string context;
    int status;
    Failure(std::string where, std::string message, int code = 1)
        : std::runtime_error(std::move(message)), context(std::move(where)), status(code) {}
};
template <class T>
T take(base::Result<T> value) {
    if (!value) {
        throw Failure(value.error().context, value.error().message);
    }
    return std::move(value).value();
}
struct Options final {
    std::string command, parent, input, tokenizer = "english", stopwords, dictionary, model, user;
    bool hmm = true, normalize_key = false;
    query::QueryMode mode = query::QueryMode::any;
    std::uint64_t version = 0, top_k = 10, window = 160, leading = 40;
    std::uint64_t query_bytes = 65536, max_k = 100, work = work_cap;
    std::uint64_t output_bytes = 16 * mib, snapshot_bytes = 512 * mib, resource_bytes = 16 * mib;
};
constexpr const char *help = R"(用法：siftwing-cli SEARCH --snapshot <父目录> --version <版本> --query <文本> [选项]
      siftwing-cli SUGGEST --snapshot <父目录> --version <版本> --key <词键> [选项]

通用选项：
  --snapshot <父目录>           快照父目录（必需）
  --version <版本>              快照版本，uint64 十进制整数（必需）
  --top-k <N>                   返回条数（默认 10）
  --help                        显示帮助，单独使用

SEARCH 选项（分词资源须与搜索建库相容）：
  --query <文本>                查询文本（必需）
  --mode and|or                 词项匹配方式（默认 or）
  --tokenizer english|cppjieba  分词器（默认 english）
  --stopwords <文件>            停用词表（可选）
  --dictionary <文件>           cppjieba 主词典（使用 cppjieba 时必需）
  --model <文件>                cppjieba HMM 模型（使用 cppjieba 时必需）
  --user-dictionary <文件>      cppjieba 用户词典（可选）
  --hmm on|off                  cppjieba HMM 开关（默认 on）
  --summary-window <N>          摘要窗口，Unicode 标量数（默认 160，1..4096）
  --summary-context <N>         摘要前文，Unicode 标量数（默认 40，须小于窗口）

SUGGEST 选项：
  --key <词键>                  词键，默认原样使用，不重新分词（必需）
  --normalize-key               将词键 ASCII 转小写并折叠空白

预算选项（正整数，仅可降低）：
  --max-query-bytes <N>         查询或词键字节上限（默认 65536）
  --max-top-k <N>               返回条数上限（默认 100）
  --max-work <N>                工作量上限（默认 50000000）
  --max-output-bytes <N>        JSON 输出字节上限（默认 16 MiB）
  --max-snapshot-bytes <N>      快照字节上限（默认 512 MiB）
  --max-resource-bytes <N>      单个分词资源字节上限（默认 16 MiB）
)";
std::uint64_t number(std::string_view value) {
    std::uint64_t result = 0;
    const auto p = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || p.ec != std::errc{} || p.ptr != value.data() + value.size()
        || std::to_string(result) != value) {
        throw Failure("cli.arguments", "需要规范uint64十进制整数", 2);
    }
    return result;
}
Options parse(int argc, char **argv) {
    if (argc < 2) {
        throw Failure("cli.arguments", "需要SEARCH或SUGGEST", 2);
    }
    Options out;
    out.command = argv[1];
    if (out.command != "SEARCH" && out.command != "SUGGEST") {
        throw Failure("cli.arguments", "命令须为SEARCH或SUGGEST", 2);
    }
    std::set<std::string> seen;
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (!seen.insert(flag).second) {
            throw Failure("cli.arguments", "重复选项：" + flag, 2);
        }
        if (flag == "--normalize-key") {
            out.normalize_key = true;
            continue;
        }
        if (++i == argc) {
            throw Failure("cli.arguments", "选项缺值：" + flag, 2);
        }
        const std::string value = argv[i];
        // 空查询/key交给业务合同，路径及其余选项不能为空。
        if (value.empty() && flag != "--query" && flag != "--key") {
            throw Failure("cli.arguments", "选项值为空：" + flag, 2);
        }
        if (flag == "--snapshot") {
            out.parent = value;
        } else if (flag == "--version") {
            out.version = number(value);
        } else if (flag == "--query" || flag == "--key") {
            out.input = value;
        } else if (flag == "--top-k") {
            out.top_k = number(value);
        } else if (flag == "--mode" && (value == "and" || value == "or")) {
            out.mode = value == "and" ? query::QueryMode::all : query::QueryMode::any;
        } else if (flag == "--tokenizer") {
            out.tokenizer = value;
        } else if (flag == "--stopwords") {
            out.stopwords = value;
        } else if (flag == "--dictionary") {
            out.dictionary = value;
        } else if (flag == "--model") {
            out.model = value;
        } else if (flag == "--user-dictionary") {
            out.user = value;
        } else if (flag == "--hmm" && (value == "on" || value == "off")) {
            out.hmm = value == "on";
        } else if (flag == "--summary-window") {
            out.window = number(value);
        } else if (flag == "--summary-context") {
            out.leading = number(value);
        } else if (flag == "--max-query-bytes") {
            out.query_bytes = number(value);
        } else if (flag == "--max-top-k") {
            out.max_k = number(value);
        } else if (flag == "--max-work") {
            out.work = number(value);
        } else if (flag == "--max-output-bytes") {
            out.output_bytes = number(value);
        } else if (flag == "--max-snapshot-bytes") {
            out.snapshot_bytes = number(value);
        } else if (flag == "--max-resource-bytes") {
            out.resource_bytes = number(value);
        } else {
            throw Failure("cli.arguments", "未知选项或非法值：" + flag, 2);
        }
    }
    if (out.parent.empty() || !seen.count("--version")
        || !seen.count(out.command == "SEARCH" ? "--query" : "--key")) {
        throw Failure("cli.arguments", "需要snapshot、version与该命令的输入", 2);
    }
    if (!out.query_bytes || out.query_bytes > 65536 || !out.max_k || out.max_k > 100 || !out.work
        || out.work > work_cap || !out.output_bytes || out.output_bytes > 16 * mib
        || !out.snapshot_bytes || out.snapshot_bytes > 512 * mib || !out.resource_bytes
        || out.resource_bytes > 16 * mib || !out.top_k || out.top_k > out.max_k) {
        throw Failure("cli.arguments", "正预算不能超过profile1；top-k须在预算内", 2);
    }
    if (out.command == "SUGGEST") {
        for (const char *flag : {"--query",
                 "--mode",
                 "--tokenizer",
                 "--stopwords",
                 "--dictionary",
                 "--model",
                 "--user-dictionary",
                 "--hmm",
                 "--summary-window",
                 "--summary-context"}) {
            if (seen.count(flag)) {
                throw Failure("cli.arguments", "SUGGEST不接纳SEARCH选项", 2);
            }
        }
    } else {
        if (seen.count("--key") || out.normalize_key) {
            throw Failure("cli.arguments", "SEARCH不接纳key选项", 2);
        }
        if (!out.window || out.window > 4096 || out.leading >= out.window) {
            throw Failure("cli.arguments", "摘要窗口须为1—4096且前文小于窗口", 2);
        }
        if (out.tokenizer == "cppjieba") {
            if (out.dictionary.empty() || out.model.empty()) {
                throw Failure("cli.arguments", "中文须显式词典与模型", 2);
            }
        } else if (out.tokenizer != "english" || !out.dictionary.empty() || !out.model.empty()
                   || !out.user.empty() || seen.count("--hmm")) {
            throw Failure("cli.arguments", "英文不接纳中文资源；tokenizer须为english或cppjieba", 2);
        }
    }
    return out;
}

// 独占只读fd；资源的路径/尺寸/变化检查不授权修改输入，也不承诺原子文件系统快照。
class File final {
public:
    explicit File(int fd) noexcept : fd_(fd) {}
    ~File() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    int get() const noexcept {
        return fd_;
    }
    void replace(int fd) noexcept {
        ::close(fd_);
        fd_ = fd;
    }
    void finish() {
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0) {
            throw Failure("cli.io", "close失败");
        }
    }

private:
    int fd_;
};
[[noreturn]] void io_error() {
    const int error = errno;
    throw Failure("cli.io", std::error_code(error, std::generic_category()).message());
}
std::string absolute(const std::string &raw) {
    const std::filesystem::path path(raw);
    for (const auto &part : path) {
        if (part == "..") {
            throw Failure("cli.path", "资源路径不能含..组件");
        }
    }
    return std::filesystem::absolute(path).lexically_normal().string();
}
std::string load(const std::string &path, std::uint64_t cap) {
    File fd(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.get() < 0) {
        io_error();
    }
    std::vector<std::string> parts;
    for (const auto &part : std::filesystem::path(path).relative_path()) {
        if (part != "." && !part.empty()) {
            parts.push_back(part.string());
        }
    }
    if (parts.empty()) {
        throw Failure("cli.path", "需要普通资源文件");
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK
                          | (i + 1 < parts.size() ? O_DIRECTORY : 0);
        int next;
        do {
            next = ::openat(fd.get(), parts[i].c_str(), flags);
        } while (next < 0 && errno == EINTR);
        if (next < 0) {
            io_error();
        }
        fd.replace(next);
    }
    struct stat before{}, after{};
    if (::fstat(fd.get(), &before) != 0) {
        io_error();
    }
    if (!S_ISREG(before.st_mode)) {
        throw Failure("cli.file", "资源须为普通文件");
    }
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > cap) {
        throw Failure("cli.resource_bytes", "装载前资源字节超限");
    }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::read(
            fd.get(), bytes.data() + done, std::min<std::size_t>(65536, bytes.size() - done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            io_error();
        }
        if (n == 0) {
            throw Failure("cli.changed", "装载期间提前EOF");
        }
        done += static_cast<std::size_t>(n);
    }
    char extra = 0;
    ssize_t tail;
    do {
        tail = ::read(fd.get(), &extra, 1);
    } while (tail < 0 && errno == EINTR);
    if (tail < 0 || ::fstat(fd.get(), &after) != 0) {
        io_error();
    }
    if (tail != 0 || before.st_size != after.st_size || before.st_dev != after.st_dev
        || before.st_ino != after.st_ino || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec
        || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        throw Failure("cli.changed", "装载期间资源变化");
    }
    fd.finish();
    return bytes;
}
struct Resource final {
    std::string path, sha;
};
struct Pipeline final {
    std::unique_ptr<text::TextPipeline> value;
    std::map<std::string, std::string> hashes;
    std::vector<Resource> resources;
};
Pipeline pipeline(const Options &o) {
    Pipeline result;
    auto resource = [&](const std::string &name, const std::string &path) {
        if (path.empty()) {
            result.hashes[name] = "absent";
            return std::string{};
        }
        const auto normalized = absolute(path);
        auto bytes = load(normalized, o.resource_bytes);
        const auto sha = take(snapshot::sha256_bytes(bytes));
        result.hashes[name] = sha;
        result.resources.push_back({normalized, sha});
        return bytes;
    };
    const auto stop = resource("stopwords", o.stopwords);
    auto words = take(text::StopWords::parse(
        o.stopwords.empty() ? std::vector<std::string_view>{} : std::vector<std::string_view>{stop},
        {16 * mib, 1000000, 4096, 64 * mib}));
    std::unique_ptr<text::Tokenizer> tokenizer;
    if (o.tokenizer == "english") {
        tokenizer = std::make_unique<text::EnglishTokenizer>();
    } else {
        (void)resource("dictionary", o.dictionary);
        (void)resource("model", o.model);
        (void)resource("user_dictionary", o.user);
        tokenizer = take(text::CppJiebaTokenizer::create({absolute(o.dictionary),
            absolute(o.model),
            o.user.empty() ? std::nullopt : std::optional<std::string>{absolute(o.user)},
            o.resource_bytes,
            o.hmm}));
    }
    result.value = take(text::TextPipeline::create(std::move(tokenizer), std::move(words)));
    return result;
}

// 专用于固定结果schema；每次追加前限额，原UTF8保留，控制字节转义，不拼入未校验JSON。
class Json final {
public:
    explicit Json(std::uint64_t cap) : cap_(cap) {}
    void raw(std::string_view bytes) {
        if (bytes.size() > cap_ - bytes_.size()) {
            throw Failure("cli.output_bytes", "完整JSON字节超限");
        }
        bytes_.append(bytes);
    }
    void string(std::string_view bytes) {
        if (!utf8::is_valid(bytes.begin(), bytes.end())) {
            throw Failure("cli.output_utf8", "显示字段不是严格UTF-8");
        }
        constexpr char digits[] = "0123456789abcdef";
        raw("\"");
        for (unsigned char c : bytes) {
            if (c == '"') {
                raw("\\\"");
            } else if (c == '\\') {
                raw("\\\\");
            } else if (c < 32) {
                const char encoded[]{'\\', 'u', '0', '0', digits[c >> 4U], digits[c & 15U]};
                raw({encoded, 6});
            } else {
                const char byte = static_cast<char>(c);
                raw({&byte, 1});
            }
        }
        raw("\"");
    }
    void integer(std::uint64_t n) {
        raw(std::to_string(n));
    }
    void boolean(bool value) {
        raw(value ? "true" : "false");
    }
    void score(double n) {
        if (!std::isfinite(n) || n <= 0 || n > 1) {
            throw Failure("cli.score", "排名分数无效");
        }
        char bytes[64];
        const auto p = std::to_chars(bytes,
            bytes + sizeof(bytes),
            n,
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (p.ec != std::errc{}) {
            throw Failure("cli.score", "分数序列化失败");
        }
        raw({bytes, static_cast<std::size_t>(p.ptr - bytes)});
    }
    const std::string &bytes() const & noexcept {
        return bytes_;
    }

private:
    std::uint64_t cap_;
    std::string bytes_;
};
const index::LexiconLimits lexicon_limits{10000000, 512 * mib, 4096, 1000000, 64 * mib};
snapshot::SnapshotReadLimits read_limits(const Options &o) {
    return {{{16 * mib, 64 * mib}, 10000, o.snapshot_bytes, 1 * mib},
        {65536, o.snapshot_bytes, 2048 * mib},
        {lexicon_limits, 10000, 1000000, 100000000, 10000},
        {lexicon_limits, 10000, 100000000, 1000000, 10000000}};
}
text::TextPipelineLimits text_limits(const Options &o) {
    return {{o.query_bytes, o.query_bytes},
        {o.query_bytes, 65536, 4096, 4 * mib},
        {65536, 4 * mib, 4096, 65536, 4 * mib, 4096, 4 * mib}};
}
void header(Json &out, const Options &o, const snapshot::Manifest &m) {
    out.raw("{\"schema_version\":1,\"command\":");
    out.string(o.command);
    out.raw(",\"snapshot_version\":");
    out.string(std::to_string(m.snapshot.value()));
    out.raw(",\"build_version\":");
    out.string(m.build_version);
    const auto &provenance =
        o.command == "SEARCH" ? m.search_provenance : m.recommendation_provenance;
    out.raw(",\"input_sha256\":");
    out.string(provenance.input_sha256);
    out.raw(",\"snapshot_config_sha256\":");
    out.string(provenance.config_sha256);
    out.raw(",\"top_k\":");
    out.integer(o.top_k);
}
std::uint64_t scalars(std::string_view bytes) {
    if (!utf8::is_valid(bytes.begin(), bytes.end())) {
        throw Failure("cli.display_utf8", "显示正文不是严格UTF-8");
    }
    return static_cast<std::uint64_t>(utf8::distance(bytes.begin(), bytes.end()));
}
void search(Json &out, const Options &o, const snapshot::SnapshotReader &reader) {
    auto p = pipeline(o);
    const auto q = take(
        query::Query::from_text(o.input, *p.value, {o.mode, o.top_k}, {text_limits(o), o.max_k}));
    const auto ranked =
        take(query::rank_top_k(q, *reader.search(), {{{4096, 10000000, 10000}, o.work}, o.max_k}));
    // 短摘要不能隐藏完整正文扫描成本；所有返回正文先共同准入，再开始任何显示。
    std::uint64_t query_scalars = 0, remaining = o.work;
    for (const auto &term : q.frequencies().terms) {
        query_scalars += scalars(term.first);
    }
    for (const auto &hit : ranked) {
        const auto *record = reader.document(hit.document_id);
        if (!record) {
            throw Failure("cli.document", "排名身份不属于同一快照");
        }
        const auto count = scalars(record->content);
        if (count > remaining / query_scalars) {
            throw Failure("cli.display_work", "全部返回正文匹配工作超限");
        }
        remaining -= count * query_scalars;
    }
    out.raw(",\"mode\":");
    out.string(o.mode == query::QueryMode::all ? "and" : "or");
    out.raw(",\"query_config\":{\"tokenizer\":");
    out.string(o.tokenizer);
    out.raw(",\"hmm\":");
    if (o.tokenizer == "cppjieba") {
        out.boolean(o.hmm);
    } else {
        out.raw("null");
    }
    for (const auto &entry : p.hashes) {
        out.raw(",");
        out.string(entry.first);
        out.raw(":");
        out.string(entry.second);
    }
    out.raw("},\"terms\":[");
    bool first = true;
    for (const auto &term : q.frequencies().terms) {
        if (!first) {
            out.raw(",");
        }
        first = false;
        out.raw("{\"term\":");
        out.string(term.first);
        out.raw(",\"tf\":");
        out.integer(term.second);
        out.raw("}");
    }
    out.raw("],\"results\":[");
    first = true;
    for (const auto &hit : ranked) {
        const auto &record = *reader.document(hit.document_id);
        const auto summary = take(query::make_summary(
            q, record.content, {o.window, o.leading}, {16 * mib, 4096, 4 * mib, o.work, 4096}));
        const auto highlight = take(query::make_highlight(
            q, summary.text, {16384, 4096, 4 * mib, o.work, 16384, 256 * 1024}));
        if (!first) {
            out.raw(",");
        }
        first = false;
        out.raw("{\"document_id\":");
        out.string(std::to_string(hit.document_id.value()));
        out.raw(",\"score\":");
        out.score(hit.score);
        out.raw(",\"title\":");
        out.string(record.title);
        out.raw(",\"url\":");
        if (record.source.url) {
            out.string(*record.source.url);
        } else {
            out.raw("null");
        }
        out.raw(",\"summary\":{\"text\":");
        out.string(summary.text);
        out.raw(",\"source_byte_offset\":");
        out.integer(summary.source_byte_offset);
        out.raw(",\"matched\":");
        out.boolean(summary.matched);
        out.raw(",\"truncated_before\":");
        out.boolean(summary.truncated_before);
        out.raw(",\"truncated_after\":");
        out.boolean(summary.truncated_after);
        out.raw(",\"html\":");
        out.string(highlight.html);
        out.raw(",\"spans\":[");
        bool span_first = true;
        for (const auto &span : highlight.spans) {
            if (!span_first) {
                out.raw(",");
            }
            span_first = false;
            out.raw("{\"byte_offset\":");
            out.integer(span.byte_offset);
            out.raw(",\"byte_length\":");
            out.integer(span.byte_length);
            out.raw("}");
        }
        out.raw("]}}");
    }
    out.raw("]");
    for (const auto &resource : p.resources) {
        if (take(snapshot::sha256_bytes(load(resource.path, o.resource_bytes))) != resource.sha) {
            throw Failure("cli.changed", "查询期间资源变化");
        }
    }
}
void suggest(Json &out, const Options &o, const snapshot::SnapshotReader &reader) {
    const auto cap = std::min<std::uint64_t>(4096, o.query_bytes);
    const auto key = o.normalize_key ? take(text::normalize_utf8(o.input, {cap, cap})) : o.input;
    const auto terms = take(query::recommend_terms(key,
        *reader.recommendation(),
        o.top_k,
        {cap, 4096, o.max_k, 10000000, 1000000, 64 * mib, o.work, 4 * mib}));
    out.raw(",\"key\":");
    out.string(key);
    out.raw(",\"normalized\":");
    out.boolean(o.normalize_key);
    out.raw(",\"results\":[");
    bool first = true;
    for (const auto &term : terms) {
        if (!first) {
            out.raw(",");
        }
        first = false;
        out.raw("{\"term_id\":");
        out.string(std::to_string(term.term_id.value()));
        out.raw(",\"term\":");
        out.string(term.term);
        out.raw(",\"distance\":");
        out.integer(term.distance);
        out.raw(",\"frequency\":");
        out.integer(term.frequency);
        out.raw("}");
    }
    out.raw("]");
}
int run(const Options &o) {
    const auto reader = take(snapshot::SnapshotReader::open(
        o.parent, take(base::SnapshotVersion::from_integer(o.version)), read_limits(o)));
    Json out(o.output_bytes);
    header(out, o, *reader.manifest());
    if (o.command == "SEARCH") {
        search(out, o, reader);
    } else {
        suggest(out, o, reader);
    }
    out.raw("}\n");
    std::cout.write(out.bytes().data(), static_cast<std::streamsize>(out.bytes().size()));
    std::cout.flush();
    if (!std::cout) {
        throw Failure("cli.output", "JSON输出失败；快照保持，只读调用可由调用方决定重试", 3);
    }
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            throw Failure("cli.signal", "无法设置本进程SIGPIPE策略");
        }
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << help;
            std::cout.flush();
            return std::cout ? 0 : 3;
        }
        return run(parse(argc, argv));
    } catch (const Failure &error) {
        std::cerr << error.context << ": " << error.what() << '\n';
        return error.status;
    } catch (const std::exception &error) {
        std::cerr << "cli.exception: " << error.what() << '\n';
        return 4;
    } catch (...) {
        std::cerr << "cli.exception: 未知异常\n";
        return 4;
    }
}
