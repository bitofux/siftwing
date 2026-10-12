/*
 * PROJECT : SIFTWING
 * FILE    : main.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 显式独立选择搜索/推荐输入，构建并无覆盖发布完整离线快照
 * IMPLEMENTATION : 规范来源顺序编号；拥有字节/TF/资源，全部建库成功后才交给Writer。
 */
#include "siftwing/document/duplicate_decision.h"
#include "siftwing/document/rss_reader.h"
#include "siftwing/document/txt_reader.h"
#include "siftwing/snapshot/writer.h"
#include "siftwing/text/cppjieba_tokenizer.h"
#include "siftwing/text/english_tokenizer.h"
#include "siftwing/text/text_pipeline.h"
#include <tinyxml2.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace siftwing;
// profile1的固定预算与可降低的五项入口预算见docs/INDEXER.md；改变固定值须升profile。
constexpr std::uint64_t mib = 1024U * 1024U;
constexpr std::uint64_t records_cap = 10000;
struct Failure final : std::runtime_error {
    std::string context;
    int status;
    Failure(std::string where, std::string message, int code = 1)
        : std::runtime_error(std::move(message)), context(std::move(where)), status(code) {}
};
template<class T> T take(base::Result<T> result) {
    if (!result) { throw Failure(result.error().context, result.error().message); }
    return std::move(result).value();
}
std::string hash(std::string_view bytes) { return take(snapshot::sha256_bytes(bytes)); }
std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : bytes) { result += digits[c >> 4U]; result += digits[c & 15U]; }
    return result;
}
std::uint64_t number(std::string_view value) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        std::to_string(result) != value) { throw Failure("indexer.arguments", "需要规范uint64十进制整数", 2); }
    return result;
}
struct Source final { document::SourceKind kind; std::string path; };
struct Branch final {
    std::vector<Source> sources;
    bool empty = false;
    std::string tokenizer = "english";
    std::string stopwords, dictionary, model, user;
    bool hmm = true;
};
struct Options final {
    Branch search, recommendation;
    std::string output, build;
    std::uint64_t version = 0, input_bytes = 16 * mib, total_input = 128 * mib;
    std::uint64_t records = records_cap, total_text = 128 * mib, comparisons = 50000000;
    std::uint32_t threshold = 3;
};
constexpr const char *help =
    "用法：siftwing-indexer --output 已有父目录 --version uint64 --build-version 稳定身份\n"
    "  --search-txt 文件 | --search-rss 文件（可重复）；无输入须 --search-empty\n"
    "  --recommendation-txt 文件 | --recommendation-rss 文件；无输入须 --recommendation-empty\n"
    "  每支可设置 --search- 或 --recommendation- 前缀的：\n"
    "    tokenizer english|cppjieba、stopwords 文件、dictionary 文件、model 文件、\n"
    "    user-dictionary 文件、hmm on|off（后三资源选项仅用于cppjieba）\n"
    "  --search-dedup-distance 0..64（默认3）\n"
    "  可降低默认预算：--max-input-bytes、--max-total-input-bytes、--max-records、\n"
    "    --max-total-text-bytes、--max-dedup-comparisons；详细合同见docs/INDEXER.md。\n";
Options parse(int argc, char **argv) {
    Options options;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        Branch *branch = nullptr;
        std::string field;
        if (flag.compare(0, 9, "--search-") == 0) { branch = &options.search; field = flag.substr(9); }
        if (flag.compare(0, 17, "--recommendation-") == 0) { branch = &options.recommendation; field = flag.substr(17); }
        const bool source = branch && (field == "txt" || field == "rss");
        if (!source && !seen.insert(flag).second) { throw Failure("indexer.arguments", "选项重复：" + flag, 2); }
        if (branch && field == "empty") { branch->empty = true; continue; }
        if (++i == argc) { throw Failure("indexer.arguments", "缺少选项值：" + flag, 2); }
        const std::string value = argv[i];
        if (value.empty()) { throw Failure("indexer.arguments", "选项值不能为空：" + flag, 2); }
        if (source) { branch->sources.push_back({field == "txt" ? document::SourceKind::txt : document::SourceKind::rss, value}); }
        else if (branch && field == "tokenizer") { branch->tokenizer = value; }
        else if (branch && field == "stopwords") { branch->stopwords = value; }
        else if (branch && field == "dictionary") { branch->dictionary = value; }
        else if (branch && field == "model") { branch->model = value; }
        else if (branch && field == "user-dictionary") { branch->user = value; }
        else if (branch && field == "hmm" && (value == "on" || value == "off")) { branch->hmm = value == "on"; }
        else if (flag == "--output") { options.output = value; }
        else if (flag == "--build-version") { options.build = value; }
        else if (flag == "--version") { options.version = number(value); }
        else if (flag == "--search-dedup-distance") {
            const auto n = number(value);
            if (n > 64) { throw Failure("indexer.arguments", "去重距离须在0—64", 2); }
            options.threshold = static_cast<std::uint32_t>(n);
        } else if (flag == "--max-input-bytes") { options.input_bytes = number(value); }
        else if (flag == "--max-total-input-bytes") { options.total_input = number(value); }
        else if (flag == "--max-records") { options.records = number(value); }
        else if (flag == "--max-total-text-bytes") { options.total_text = number(value); }
        else if (flag == "--max-dedup-comparisons") { options.comparisons = number(value); }
        else { throw Failure("indexer.arguments", "未知选项或非法值：" + flag, 2); }
    }
    if (options.output.empty() || options.build.empty() || !seen.count("--version")) {
        throw Failure("indexer.arguments", "必须提供output、version和build-version", 2);
    }
    if (options.build.size() > 128 || options.build.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") != std::string::npos) {
        throw Failure("indexer.arguments", "build-version须为1—128字节ASCII稳定身份", 2);
    }
    if (!options.input_bytes || options.input_bytes > 16*mib || !options.total_input || options.total_input > 128*mib ||
        !options.records || options.records > records_cap || !options.total_text || options.total_text > 128*mib ||
        !options.comparisons || options.comparisons > 50000000) {
        throw Failure("indexer.arguments", "预算须为正且不能超过profile1上界", 2);
    }
    for (auto *branch : {&options.search, &options.recommendation}) {
        if (branch->sources.empty() != branch->empty) {
            throw Failure("indexer.arguments", "每支选择文件或显式empty，二者不能混用", 2);
        }
        if (branch->tokenizer != "english" && branch->tokenizer != "cppjieba") {
            throw Failure("indexer.arguments", "分词器须为english或cppjieba", 2);
        }
        if (branch->tokenizer == "cppjieba") {
            if (branch->dictionary.empty() || branch->model.empty()) {
                throw Failure("indexer.arguments", "cppjieba须显式提供dictionary和model", 2);
            }
        } else if (!branch->dictionary.empty() || !branch->model.empty() || !branch->user.empty() ||
                   seen.count(branch == &options.search ? "--search-hmm" : "--recommendation-hmm")) {
            throw Failure("indexer.arguments", "英文分词不接纳cppjieba资源选项", 2);
        }
    }
    return options;
}

// Linux只读fd独占RAII；路径逐组件openat，无符号链接或非常规文件的回退。
class File final {
public:
    explicit File(int descriptor) noexcept : descriptor_(descriptor) {}
    ~File() { if (descriptor_ >= 0) { ::close(descriptor_); } }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    int get() const noexcept { return descriptor_; }
    void replace(int descriptor) noexcept { ::close(descriptor_); descriptor_ = descriptor; }
    void finish() {
        const auto fd = descriptor_; descriptor_ = -1;
        if (::close(fd) != 0) { throw Failure("indexer.io", "只读文件close失败"); }
    }
private:
    int descriptor_;
};
[[noreturn]] void io_error(const std::string &path) {
    const auto error = errno;
    throw Failure("indexer.io", path + ": " + std::error_code(error, std::generic_category()).message());
}
std::string absolute(const std::string &path) {
    if (path.empty() || path.find('\0') != path.npos) { throw Failure("indexer.path", "文件路径非空且不能含NUL"); }
    const std::filesystem::path parsed(path);
    for (const auto &part : parsed) { if (part == "..") { throw Failure("indexer.path", "文件路径不能含..组件"); } }
    return std::filesystem::absolute(parsed).lexically_normal().string();
}
std::string load(const std::string &path, std::uint64_t limit) {
    const std::filesystem::path parsed(path);
    File fd(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.get() < 0) { io_error(path); }
    std::vector<std::string> components;
    for (const auto &part : parsed.relative_path()) { if (part != "." && !part.empty()) { components.push_back(part.string()); } }
    if (components.empty()) { throw Failure("indexer.path", "需要普通文件路径"); }
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
            (i + 1 < components.size() ? O_DIRECTORY : 0);
        int next;
        do { next = ::openat(fd.get(), components[i].c_str(), flags); } while (next < 0 && errno == EINTR);
        if (next < 0) { io_error(path); }
        fd.replace(next);
    }
    struct stat before{}, after{};
    if (::fstat(fd.get(), &before) != 0) { io_error(path); }
    if (!S_ISREG(before.st_mode)) { throw Failure("indexer.file", "只接纳普通文件：" + path); }
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > limit) {
        throw Failure("indexer.input_bytes", "装载前文件字节超限：" + path);
    }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::read(fd.get(), bytes.data() + done, std::min<std::size_t>(65536, bytes.size() - done));
        if (count < 0 && errno == EINTR) { continue; }
        if (count < 0) { io_error(path); }
        if (count == 0) { throw Failure("indexer.changed", "装载期间提前EOF：" + path); }
        done += static_cast<std::size_t>(count);
    }
    char extra = 0;
    ssize_t tail;
    do { tail = ::read(fd.get(), &extra, 1); } while (tail < 0 && errno == EINTR);
    if (tail < 0 || ::fstat(fd.get(), &after) != 0) { io_error(path); }
    if (tail != 0 || before.st_size != after.st_size || before.st_ino != after.st_ino || before.st_dev != after.st_dev ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        throw Failure("indexer.changed", "装载期间来源变化：" + path);
    }
    fd.finish();
    return bytes;
}

bool unnamespaced(const tinyxml2::XMLElement *element, std::string_view name) {
    if (!element || std::string_view(element->Name()) != name) { return false; }
    for (auto *scope = element; scope; scope = scope->Parent() ? scope->Parent()->ToElement() : nullptr) {
        if (const auto *uri = scope->Attribute("xmlns")) { return *uri == '\0'; }
    }
    return true;
}
std::size_t rss_items(std::string_view bytes, std::uint64_t remaining) {
    // 这里只数原item以准备身份，不代替RssDocumentReader的编码/实体/命名空间/正文验证。
    // tinyxml2把PI当声明；在注释/CDATA外将非XML声明PI换为空注释，避免合法PI改变编号。
    std::string prepared;
    for (std::size_t i = 0; i < bytes.size();) {
        std::string_view closing;
        if (bytes.substr(i, 4) == "<!--") { closing = "-->"; }
        else if (bytes.substr(i, 9) == "<![CDATA[") { closing = "]]>"; }
        if (!closing.empty()) {
            const auto end = bytes.find(closing, i + closing.size());
            if (end == bytes.npos) { return 0; }
            prepared.append(bytes.substr(i, end + closing.size() - i)); i = end + closing.size();
        } else if (bytes.substr(i, 2) == "<?") {
            const auto end = bytes.find("?>", i + 2);
            if (end == bytes.npos) { return 0; }
            if (bytes.substr(i, 5) == "<?xml" && (i == 0 || (i == 3 && bytes.substr(0, 3) == "\xef\xbb\xbf"))) {
                prepared.append(bytes.substr(i, end + 2 - i));
            } else { prepared += "<!---->"; }
            i = end + 2;
        } else { prepared += bytes[i++]; }
    }
    tinyxml2::XMLDocument xml;
    if (xml.Parse(prepared.data(), prepared.size()) != tinyxml2::XML_SUCCESS) { return 0; }
    const auto *root = xml.RootElement();
    if (!unnamespaced(root, "rss")) { return 0; }
    const tinyxml2::XMLElement *channel = nullptr;
    for (auto *child = root->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if (unnamespaced(child, "channel")) { if (channel) { return 0; } channel = child; }
    }
    if (!channel) { return 0; }
    std::size_t count = 0;
    for (auto *item = channel->FirstChildElement(); item; item = item->NextSiblingElement()) {
        if (unnamespaced(item, "item")) {
            if (count >= remaining) { throw Failure("indexer.records", "原RSS item身份预算超限"); }
            ++count;
        }
    }
    return count;
}
struct Resource final { std::string path, sha; };
struct Analysis final {
    std::vector<document::DocumentRecord> documents;
    std::vector<text::DocumentTermFrequencies> frequencies;
    std::vector<Resource> resources;
    document::ReadCounts counts;
    snapshot::Provenance provenance;
    std::uint64_t input_files = 0, records = 0, text_bytes = 0;
};
const text::TextPipelineLimits text_limits{{16*mib,16*mib}, {16*mib,1000000,4096,64*mib},
    {1000000,64*mib,4096,1000000,64*mib,1000000,64*mib}};
const index::LexiconLimits lexicon_limits{10000000,512*mib,4096,1000000,64*mib};
const index::InvertedIndexLimits index_limits{lexicon_limits,records_cap,1000000,100000000,records_cap};
const index::RecommendationDictionaryLimits dictionary_limits{lexicon_limits,records_cap,100000000,1000000,10000000};
const snapshot::SnapshotWriteLimits write_limits{{{16*mib,64*mib},records_cap,512*mib,1*mib},
    {65536,512*mib,2048*mib}};
Analysis analyze(Branch branch, const Options &options, const std::string &role) {
    Analysis result;
    for (auto &source : branch.sources) { source.path = absolute(source.path); }
    std::sort(branch.sources.begin(), branch.sources.end(), [](const Source &a, const Source &b) { return a.path < b.path; });
    for (std::size_t i = 1; i < branch.sources.size(); ++i) {
        if (branch.sources[i-1].path == branch.sources[i].path) { throw Failure("indexer.sources", "分支内来源重复"); }
    }
    if (branch.sources.size() > options.records) { throw Failure("indexer.records", "输入文件数量超限"); }
    std::ostringstream config;
    config << "SIFTWING-INDEXER-CONFIG 1\nrole " << role << "\nprofile 1\n"
        << "reader txt1 rss1 partial-reject body-only\nnormalize 1\nlexicon 1\n"
        << "tokenizer " << branch.tokenizer << " 1\ninput-bytes " << options.input_bytes
        << "\ntotal-input-bytes " << options.total_input << "\nrecords " << options.records
        << "\ntotal-text-bytes " << options.total_text << '\n';
    if (role == "search") { config << "simhash 1\ndedup " << options.threshold << ' ' << options.comparisons << "\nweights 1\n"; }
    else { config << "accumulation 1 all-delivered\n"; }
    auto resource = [&](const std::string &name, const std::string &path) {
        if (path.empty()) { config << name << " absent\n"; return std::string{}; }
        const auto normalized = absolute(path);
        auto bytes = load(normalized, options.input_bytes);
        const auto digest = hash(bytes);
        result.resources.push_back({normalized,digest}); config << name << ' ' << digest << '\n';
        return bytes;
    };
    const auto stop_bytes = resource("stopwords", branch.stopwords);
    auto stops = take(text::StopWords::parse(branch.stopwords.empty() ? std::vector<std::string_view>{} :
        std::vector<std::string_view>{stop_bytes}, {16*mib,1000000,4096,64*mib}));
    std::unique_ptr<text::Tokenizer> tokenizer;
    if (branch.tokenizer == "english") { tokenizer = std::make_unique<text::EnglishTokenizer>(); }
    else {
        (void)resource("dictionary", branch.dictionary); (void)resource("model", branch.model);
        (void)resource("user-dictionary", branch.user); config << "hmm " << (branch.hmm ? "on" : "off") << '\n';
        tokenizer = take(text::CppJiebaTokenizer::create({absolute(branch.dictionary), absolute(branch.model),
            branch.user.empty() ? std::nullopt : std::optional<std::string>{absolute(branch.user)},options.input_bytes,branch.hmm}));
    }
    const auto pipeline = take(text::TextPipeline::create(std::move(tokenizer), std::move(stops)));
    std::ostringstream inputs;
    inputs << "SIFTWING-INDEXER-INPUTS 1\n";
    std::uint64_t loaded_bytes = 0, next_id = 0;
    for (const auto &source : branch.sources) {
        if (result.records == options.records || result.text_bytes == options.total_text) {
            throw Failure("indexer.records", "批量读取预算耗尽，未处理后续来源");
        }
        // 装载前以剩余累计预算和单文件预算的较小值限制分配，原BOM/换行均计入。
        const auto bytes = load(source.path, std::min(options.input_bytes, options.total_input - loaded_bytes));
        loaded_bytes += static_cast<std::uint64_t>(bytes.size());
        const auto logical = source.path.substr(1);
        inputs << "F " << (source.kind == document::SourceKind::txt ? "txt" : "rss") << ' ' << hex(logical)
            << ' ' << bytes.size() << ' ' << hash(bytes) << '\n';
        std::unique_ptr<document::DocumentReader> reader;
        if (source.kind == document::SourceKind::txt) {
            reader = std::make_unique<document::TxtDocumentReader>(
                std::map<std::string,document::TxtDocumentMetadata>{{logical,{take(base::DocumentId::from_integer(next_id++)),std::nullopt}}});
        } else {
            const auto count = rss_items(bytes, options.records - result.records);
            std::vector<base::DocumentId> ids;
            for (std::size_t i = 0; i < count; ++i) { ids.push_back(take(base::DocumentId::from_integer(next_id++))); }
            if (count == 0) { ++next_id; }
            reader = std::make_unique<document::RssDocumentReader>(
                std::map<std::string,std::vector<base::DocumentId>>{{logical,std::move(ids)}},
                document::RssParseLimits{1000000,128,{16*mib,16*mib,1000000,1000000,128}});
        }
        auto report = take(document::read_documents({{source.kind,logical,bytes}},*reader,
            {options.input_bytes,16*mib,options.records-result.records,options.total_text-result.text_bytes},{}));
        for (auto &item : report.items) {
            for (const auto &issue : item.issues) {
                std::cerr << role << ':' << hex(item.source_id.input_path) << ':' << item.source_id.record_ordinal
                    << ':' << issue.region << ": " << issue.message << '\n';
            }
            if (item.record) {
                auto analysis = take(pipeline->analyze(item.record->content, text_limits));
                result.frequencies.push_back(std::move(analysis.tf)); result.documents.push_back(std::move(*item.record));
            }
        }
        if (report.stop || report.counts.rejected || report.counts.failed) {
            throw Failure("indexer.read", role + "输入未完整交付，停止建库");
        }
        result.counts.success += report.counts.success; result.counts.warning += report.counts.warning;
        result.counts.no_text += report.counts.no_text;
        result.records += static_cast<std::uint64_t>(report.items.size()); result.text_bytes += report.total_text_bytes;
    }
    result.input_files = static_cast<std::uint64_t>(branch.sources.size());
    result.provenance = {hash(inputs.str()),hash(config.str())};
    return result;
}
void statistics(std::ostringstream &out, const char *role, const Analysis &analysis) {
    out << role << ".files=" << analysis.input_files << '\n' << role << ".read_success=" << analysis.counts.success
        << '\n' << role << ".read_warning=" << analysis.counts.warning << '\n' << role << ".no_text="
        << analysis.counts.no_text << '\n' << role << ".delivered=" << analysis.documents.size() << '\n'
        << role << ".input_sha256=" << analysis.provenance.input_sha256 << '\n'
        << role << ".config_sha256=" << analysis.provenance.config_sha256 << '\n';
}
int run(const Options &options) {
    auto search = analyze(options.search, options, "search");
    auto recommendation = analyze(options.recommendation, options, "recommendation");
    std::vector<document::DuplicateCandidate> candidates;
    for (std::size_t i = 0; i < search.documents.size(); ++i) {
        const auto fingerprint = take(document::make_simhash(search.frequencies[i],{1000000,4096,64*mib,1000000}));
        candidates.push_back({search.documents[i].doc_id,search.documents[i].source.source_id,fingerprint});
    }
    const auto duplicates = take(document::decide_duplicates(candidates,{options.threshold},
        {options.records,4096,64*mib,options.comparisons}));
    std::set<base::DocumentId> kept;
    for (const auto &decision : duplicates.decisions) {
        if (decision.disposition == document::DuplicateDisposition::kept) { kept.insert(decision.doc_id); }
    }
    std::vector<document::DocumentRecord> documents;
    std::vector<index::IndexDocument> tf;
    for (std::size_t i = 0; i < search.documents.size(); ++i) {
        if (kept.count(search.documents[i].doc_id)) {
            tf.push_back({search.documents[i].doc_id,std::move(search.frequencies[i])});
            documents.push_back(std::move(search.documents[i]));
        }
    }
    // 移动保留集只发生在诊断/去重已完成之后；推荐始终消费自己的全部交付TF。
    auto weighted = take(index::WeightedIndex::build(take(index::InvertedIndex::build(tf,index_limits)),
        {records_cap,1000000,10000000}));
    text::RecommendationFrequencies accumulated;
    for (const auto &frequencies : recommendation.frequencies) {
        accumulated = take(text::accumulate_recommendation(accumulated,frequencies,{options.records,1000000,4096,64*mib,100000000}));
    }
    const auto dictionary = take(index::RecommendationDictionary::build(accumulated,dictionary_limits));
    // cppjieba工厂独立读文件；发布前重核实际资源字节。调用期稳定仍是前置条件，非敌对原子快照。
    for (const auto *branch : {&search,&recommendation}) {
        for (const auto &resource : branch->resources) {
            if (hash(load(resource.path,options.input_bytes)) != resource.sha) {
                throw Failure("indexer.changed", "资源在建库期间变化：" + resource.path);
            }
        }
    }
    const auto &raw = weighted.raw_index();
    const auto totals = raw.statistics();
    std::ostringstream out;
    out << "snapshot=" << options.version << '\n'; statistics(out,"search",search); statistics(out,"recommendation",recommendation);
    out << "search.kept=" << duplicates.counts.kept << "\nsearch.duplicates=" << duplicates.counts.duplicates
        << "\nsearch.no_features=" << duplicates.counts.no_features << "\nsearch.comparisons=" << duplicates.counts.comparisons
        << "\nsearch.documents=" << totals.documents << "\nsearch.terms=" << raw.lexicon().terms().size()
        << "\nsearch.total_tokens=" << totals.total_tokens << "\nsearch.postings=" << totals.total_postings
        << "\nrecommendation.documents=" << dictionary.documents() << "\nrecommendation.terms=" << dictionary.lexicon().terms().size()
        << "\nrecommendation.total_tokens=" << dictionary.total_tokens() << '\n';
    const auto report = out.str(); // 所有报告分配在发布之前完成，发布后不重试/删除正式目录。
    const auto published = take(snapshot::write_snapshot(options.output,documents,weighted,dictionary,
        {take(base::SnapshotVersion::from_integer(options.version)),options.build,search.provenance,recommendation.provenance},write_limits));
    std::cout << report << "durability=" << (published.durability == snapshot::PublicationDurability::synced ? "synced" : "unconfirmed")
        << "\nsystem_errno=" << published.system_errno << '\n' << std::flush;
    if (!std::cout) { std::cerr << "indexer.report: 快照已发布，统计报告写出失败\n"; return 4; }
    return published.durability == snapshot::PublicationDurability::synced ? 0 : 3;
}
} // namespace

int main(int argc, char **argv) {
    // 统计输出遇关闭管道转为流错误，以明确报告“已发布”，不由SIGPIPE隐藏发布结果。
    if (std::signal(SIGPIPE,SIG_IGN) == SIG_ERR) { return 4; }
    if (argc == 2 && std::string_view(argv[1]) == "--help") { std::cout << help; return std::cout ? 0 : 4; }
    try { return run(parse(argc,argv)); }
    catch (const Failure &error) { std::cerr << error.context << ": " << error.what() << '\n'; return error.status; }
    catch (const std::exception &error) { std::cerr << "indexer.exception: " << error.what() << '\n'; return 4; }
    catch (...) { std::cerr << "indexer.exception: 未知异常\n"; return 4; }
}
