/*
 * PROJECT : SIFTWING
 * FILE    : search_service.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 装载时固定资源，调用局部解析/计算/有限JSON输出
 * IMPLEMENTATION : 组合既有V1核心；资源只在初始化读取，结果无输入或连接借用。
 */
#include "siftwing/query/search_service.hpp"
#include <nlohmann/json.hpp>
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

namespace siftwing::query {
namespace {
using namespace siftwing;
using Status = net::ErrorStatus;
constexpr std::uint64_t mib = 1024U * 1024U;
struct Failure final : std::runtime_error {
    std::string context;
    Status status;
    Failure(std::string where, std::string message, Status code = Status::internal_error)
        : std::runtime_error(std::move(message)), context(std::move(where)), status(code) {}
};
// 显式登记本轮复用核心的业务/预算诊断；未知诊断保持INTERNAL_ERROR，绝不按文案猜类型。
Status classify(std::string_view context) {
    constexpr std::string_view invalid[]{"query.empty", "query.mode", "query.top_k",
        "normalize.nul", "tokenizer.nul", "recommend.query", "recommend.nul",
        "recommend.top_k", "summary.options"};
    constexpr std::string_view resource[]{"normalize.input_bytes", "normalize.output_bytes",
        "tokenizer.input_bytes", "tokenizer.token_bytes", "tokenizer.tokens", "tokenizer.output_bytes",
        "frequency.input_bytes", "frequency.input_tokens", "frequency.token_bytes",
        "frequency.output_bytes", "frequency.output_tokens", "frequency.distinct_terms",
        "frequency.vocabulary_bytes", "candidate.terms", "candidate.postings", "candidate.documents",
        "ranking.work", "summary.content_bytes", "summary.query_terms", "summary.query_bytes", "summary.work",
        "highlight.text_bytes", "highlight.query_terms", "highlight.query_bytes", "highlight.work",
        "highlight.spans", "highlight.output_bytes", "recommend.query_bytes", "recommend.query_scalars",
        "recommend.character_links", "recommend.candidates", "recommend.candidate_bytes",
        "recommend.work", "recommend.output_bytes"};
    for (auto name : invalid) if (context == name) return Status::invalid_request;
    for (auto name : resource) if (context == name) return Status::resource_limit;
    return Status::internal_error;
}
template <class T> T take(base::Result<T> value) {
    if (!value) throw Failure(value.error().context, value.error().message, classify(value.error().context));
    return std::move(value).value();
}
struct Options : SearchServiceConfig {
    std::string command, input;
    bool normalize_key = false;
    QueryMode mode = QueryMode::any;
    std::uint64_t top_k = 10, window = 160, leading = 40;
};
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
            throw Failure("service.io", "close失败");
        }
    }

private:
    int fd_;
};
[[noreturn]] void io_error() {
    const int error = errno;
    throw Failure("service.io", std::error_code(error, std::generic_category()).message());
}
std::string absolute(const std::string &raw) {
    if (raw.find('\0') != raw.npos) throw Failure("service.path", "资源路径含NUL");
    const std::filesystem::path path(raw);
    for (const auto &part : path) {
        if (part == "..") {
            throw Failure("service.path", "资源路径不能含..组件");
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
        throw Failure("service.path", "需要普通资源文件");
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
        throw Failure("service.file", "资源须为普通文件");
    }
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > cap) {
        throw Failure("service.resource_bytes", "装载前资源字节超限");
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
            throw Failure("service.changed", "装载期间提前EOF");
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
        throw Failure("service.changed", "装载期间资源变化");
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
            throw Failure("service.output_bytes", "完整JSON字节超限", Status::resource_limit);
        }
        bytes_.append(bytes);
    }
    void string(std::string_view bytes) {
        if (!utf8::is_valid(bytes.begin(), bytes.end())) {
            throw Failure("service.output_utf8", "显示字段不是严格UTF-8");
        }
        constexpr char digits[] = "0123456789abcdef";
        raw("\"");
        for (char byte_value : bytes) {
            const auto c = static_cast<unsigned char>(byte_value);
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
            throw Failure("service.score", "排名分数无效");
        }
        char bytes[64];
        const auto p = std::to_chars(bytes,
            bytes + sizeof(bytes),
            n,
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (p.ec != std::errc{}) {
            throw Failure("service.score", "分数序列化失败");
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
        throw Failure("service.display_utf8", "显示正文不是严格UTF-8");
    }
    return static_cast<std::uint64_t>(utf8::distance(bytes.begin(), bytes.end()));
}
void search(Json &out, const Options &o, const snapshot::SnapshotReader &reader, const Pipeline &p) {
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
            throw Failure("service.document", "排名身份不属于同一快照");
        }
        const auto count = scalars(record->content);
        if (count > remaining / query_scalars) {
            throw Failure("service.display_work", "全部返回正文匹配工作超限", Status::resource_limit);
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

[[noreturn]] void arguments() { throw Failure("service.arguments", "非法业务字段", Status::invalid_request); }
Options parse_request(const Options &fixed, const concurrency::SearchTask &task) {
    Options o = fixed;
    o.output_bytes = std::min<std::uint64_t>(o.output_bytes, task.body_limit());
    const bool search_request = task.request()->message_type == net::MessageType::search_request;
    o.command = search_request ? "SEARCH" : "SUGGEST";
    // codec已经核严格UTF8/对象/64层；业务DOM额外拒重复键，避免last-wins歧义。
    std::set<std::string> keys;
    bool duplicate = false;
    const auto callback = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json &value) {
        if (event == nlohmann::json::parse_event_t::key && depth == 1)
            if (!keys.insert(value.get<std::string>()).second) duplicate = true;
        return true;
    };
    const auto j = nlohmann::json::parse(task.request()->body, callback);
    if (duplicate || !j.is_object()) arguments();
    const std::set<std::string> allowed = search_request
        ? std::set<std::string>{"query", "mode", "top_k", "summary_window", "summary_context"}
        : std::set<std::string>{"key", "top_k", "normalize_key"};
    for (const auto &item : j.items()) if (!allowed.count(item.key())) arguments();
    const auto text = search_request ? "query" : "key";
    if (!j.contains(text) || !j[text].is_string()) arguments();
    o.input = j[text].get<std::string>();
    auto integer = [&](const char *name, std::uint64_t &value) {
        if (!j.contains(name)) return;
        if (!j[name].is_number_unsigned()) arguments();
        value = j[name].get<std::uint64_t>();
    };
    integer("top_k", o.top_k);
    if (o.top_k == 0 || o.top_k > o.max_k) arguments();
    if (search_request) {
        if (j.contains("mode")) {
            if (!j["mode"].is_string()) arguments();
            const auto mode = j["mode"].get<std::string>();
            if (mode != "and" && mode != "or") arguments();
            o.mode = mode == "and" ? QueryMode::all : QueryMode::any;
        }
        integer("summary_window", o.window); integer("summary_context", o.leading);
        if (o.window == 0 || o.window > 4096 || o.leading >= o.window) arguments();
    } else if (j.contains("normalize_key")) {
        if (!j["normalize_key"].is_boolean()) arguments();
        o.normalize_key = j["normalize_key"].get<bool>();
    }
    return o;
}
void validate_config(const SearchServiceConfig &o) {
    if (o.cache.capacity > 4096 || o.cache.bytes == 0 || o.cache.bytes > 64 * mib
        || o.cache.max_value_bytes == 0 || o.cache.max_value_bytes > mib
        || o.cache.max_value_bytes > o.cache.bytes)
        throw Failure("service.cache_config", "非法缓存配置");
    if (o.parent.empty() || !o.query_bytes || o.query_bytes > 65536 || !o.max_k || o.max_k > 100
        || !o.work || o.work > 50000000 || !o.output_bytes || o.output_bytes > net::frame_max_body_bytes
        || !o.snapshot_bytes || o.snapshot_bytes > 512 * mib || !o.resource_bytes || o.resource_bytes > 16 * mib
        || (o.tokenizer != "english" && o.tokenizer != "cppjieba")
        || (o.tokenizer == "english" && (!o.dictionary.empty() || !o.model.empty() || !o.user.empty()))
        || (o.tokenizer == "cppjieba" && (o.dictionary.empty() || o.model.empty())))
        throw Failure("service.config", "非法固定服务配置");
}
} // namespace
struct SearchService::Impl {
    Options options;
    snapshot::SnapshotReader reader;
    Pipeline pipeline;
    cache::VersionedLru cache;
    Impl(Options selected, snapshot::SnapshotReader loaded, Pipeline resources)
        : options(std::move(selected)), reader(std::move(loaded)), pipeline(std::move(resources)),
          cache(options.cache) {}
};
SearchService::SearchService(std::unique_ptr<Impl> state) noexcept : state_(std::move(state)) {}
SearchService::~SearchService() noexcept = default;
base::Result<std::unique_ptr<SearchService>> SearchService::create(SearchServiceConfig config) {
    using Output = base::Result<std::unique_ptr<SearchService>>;
    try {
        validate_config(config);
        Options o; static_cast<SearchServiceConfig &>(o) = std::move(config);
        auto reader = take(snapshot::SnapshotReader::open(o.parent,
            take(base::SnapshotVersion::from_integer(o.version)), read_limits(o)));
        auto p = pipeline(o);
        // cppjieba工厂独立读取已验证文件；重核SHA后才发布组合，不将路径共享给worker。
        for (const auto &resource : p.resources)
            if (take(snapshot::sha256_bytes(load(resource.path, o.resource_bytes))) != resource.sha)
                throw Failure("service.changed", "初始化期间资源变化");
        auto state = std::make_unique<Impl>(std::move(o), std::move(reader), std::move(p));
        return Output::success(std::unique_ptr<SearchService>(new SearchService(std::move(state))));
    } catch (const Failure &error) {
        return Output::failure({error.what(), error.context});
    }
}
SearchServiceResult SearchService::execute(
    const concurrency::SearchTask &task, CacheObservation *observation) const {
    const auto observe = [observation](CacheObservation value) noexcept {
        if (observation)
            *observation = value;
    };
    observe(CacheObservation::not_checked);
    if (!task.valid()) return Status::invalid_request;
    try {
        const auto o = parse_request(state_->options, task);
        Json out(o.output_bytes); header(out, o, *state_->reader.manifest());
        std::optional<cache::CacheKey> key;
        if (o.cache.capacity == 0)
            observe(CacheObservation::disabled);
        if (o.cache.capacity != 0) {
            // schema与原始文本预算先检查；同一规范化键不能替超长原请求绕过准入。
            const bool is_search = o.command == "SEARCH";
            const auto cap = is_search ? o.query_bytes : std::min<std::uint64_t>(4096, o.query_bytes);
            if (o.input.size() > cap)
                throw Failure("service.query_bytes", "原查询字节超限", Status::resource_limit);
            auto canonical = is_search || o.normalize_key
                ? take(text::normalize_utf8(o.input, {cap, cap})) : o.input;
            // JSON数组按解析后的有效参数编码，键序/省略默认值等价，字符串不会有拼接歧义。
            const auto params = is_search
                ? nlohmann::json::array({o.top_k, o.mode == QueryMode::all, o.window, o.leading, o.output_bytes})
                : nlohmann::json::array({o.top_k, o.normalize_key, o.output_bytes});
            key = cache::CacheKey{state_->reader.manifest()->snapshot.value(),
                is_search ? cache::Operation::search : cache::Operation::suggest,
                std::move(canonical), params.dump()};
            observe(CacheObservation::miss);
            try {
                if (auto hit = state_->cache.get(*key)) {
                    observe(CacheObservation::hit);
                    return std::move(*hit);
                }
            } catch (const std::bad_alloc &) {
                observe(CacheObservation::unavailable);
            }
        }
        if (o.command == "SEARCH") search(out, o, state_->reader, state_->pipeline);
        else suggest(out, o, state_->reader);
        out.raw("}");
        if (key) {
            try { (void)state_->cache.put(*key, out.bytes()); }
            catch (const std::bad_alloc &) { /* 可选缓存分配失败不使已完成业务变成错误。 */ }
        }
        return out.bytes();
    } catch (const Failure &error) { return error.status; }
    catch (const nlohmann::json::parse_error &) { return Status::invalid_request; }
}
std::string SearchService::server_info() const {
    const auto &o = state_->options;
    const auto &m = *state_->reader.manifest();
    return nlohmann::json{{"schema_version", 1}, {"command", "SERVER_INFO"},
        {"protocol_version", 1}, {"snapshot_version", std::to_string(m.snapshot.value())},
        {"build_version", m.build_version},
        {"commands", {"PING", "SERVER_INFO", "SEARCH", "SUGGEST"}},
        {"limits", {{"max_body_bytes", o.output_bytes}, {"max_query_bytes", o.query_bytes}, {"max_top_k", o.max_k}}},
        {"cache_enabled", o.cache.capacity != 0}}.dump();
}
std::uint64_t SearchService::index_version() const noexcept {
    return state_->reader.manifest()->snapshot.value();
}
cache::LruStats SearchService::cache_stats() const { return state_->cache.stats(); }
} // namespace siftwing::query
