/*
 * PROJECT : SIFTWING
 * FILE    : cppjieba_tokenizer.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 将可诊断资源加载和有界拥有型分词隔离在cppjieba适配层
 * IMPLEMENTATION :
 * -- 文件只读装载一次，严格验证后的内存记录进入库，避开路径重读/缓存/fatal检查
 * -- segment借用自身Impl的词典/模型，按成员逆序销毁
 * -- 汉字段内部仍由成熟DAG/HMM分词；输出span先计预算，整体通过后复制拥有string
 */

#include "siftwing/text/cppjieba_tokenizer.h"
#include "siftwing/base/checked.h"

#include <cppjieba/MixSegment.hpp>
#include <utf8.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace siftwing::text {
namespace {

struct InitFailure final {
    base::Error error;
};
[[noreturn]] void reject(std::string context, std::string message) {
    throw InitFailure{{std::move(message), std::move(context)}};
}

class FileDescriptor final {
  public:
    explicit FileDescriptor(int value) noexcept : value_(value) {}
    ~FileDescriptor() noexcept {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
    }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }
    void replace(int value) noexcept {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
        value_ = value;
    }

  private:
    int value_;
};

bool valid_path(std::string_view path) noexcept {
    if (path.size() < 2 || path.front() != '/' || path.back() == '/' ||
        path.find('\0') != path.npos) {
        return false;
    }
    std::size_t begin = 1;
    while (begin < path.size()) {
        auto end = path.find('/', begin);
        if (end == path.npos) {
            end = path.size();
        }
        const auto component = path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        begin = end + 1;
    }
    return true;
}

std::string load_file(const std::string &path, std::uint64_t limit, const char *label) {
    FileDescriptor directory(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) {
        reject("cppjieba.io", std::string(label) + ": cannot open root directory");
    }
    std::size_t begin = 1;
    for (;;) {
        auto end = path.find('/', begin);
        const bool last = end == path.npos;
        if (last) {
            end = path.size();
        }
        const std::string component = path.substr(begin, end - begin);
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? O_NONBLOCK : O_DIRECTORY);
        const int fd = ::openat(directory.get(), component.c_str(), flags);
        if (fd < 0) {
            reject("cppjieba.io",
                   std::string(label) + ": open failed, errno=" + std::to_string(errno));
        }
        directory.replace(fd);
        if (last) {
            break;
        }
        begin = end + 1;
    }
    struct stat before {};
    if (::fstat(directory.get(), &before) != 0 || !S_ISREG(before.st_mode)) {
        reject("cppjieba.io", std::string(label) + ": resource must be a regular file");
    }
    const auto size = base::checked_narrow<std::uint64_t>(before.st_size, "cppjieba.file_bytes");
    if (!size || size.value() > limit) {
        reject("cppjieba.file_bytes", std::string(label) + ": input exceeds file limit");
    }
    const auto reserve = base::checked_narrow<std::size_t>(size.value(), "cppjieba.file_bytes");
    if (!reserve) {
        reject("cppjieba.file_bytes", std::string(label) + ": file size cannot be represented");
    }
    std::string bytes;
    bytes.reserve(reserve.value());
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(directory.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            reject("cppjieba.io",
                   std::string(label) + ": read failed, errno=" + std::to_string(errno));
        }
        if (count == 0) {
            break;
        }
        const auto n = static_cast<std::size_t>(count);
        if (n > limit - bytes.size()) {
            reject("cppjieba.file_bytes", std::string(label) + ": grew beyond file limit");
        }
        bytes.append(buffer.data(), n);
    }
    struct stat after {};
    if (::fstat(directory.get(), &after) != 0 || before.st_size != after.st_size ||
        bytes.size() != size.value() || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        reject("cppjieba.io", std::string(label) + ": file changed during initialization");
    }
    const auto invalid = utf8::find_invalid(bytes.begin(), bytes.end());
    if (invalid != bytes.end()) {
        reject("cppjieba.utf8", std::string(label) + ": invalid UTF-8 at byte " +
                                    std::to_string(invalid - bytes.begin()));
    }
    const auto nul = bytes.find('\0');
    if (nul != bytes.npos) {
        reject("cppjieba.nul", std::string(label) + ": NUL at byte " + std::to_string(nul));
    }
    return bytes;
}

std::string_view trim_ascii(std::string_view text) {
    constexpr std::string_view space = " \t\r\n";
    const auto first = text.find_first_not_of(space);
    if (first == text.npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

std::vector<std::string_view> split(std::string_view text, char separator,
                                    bool keep_empty = false) {
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        auto end = text.find(separator, begin);
        if (end == text.npos) {
            end = text.size();
        }
        if (end != begin || keep_empty) {
            parts.push_back(text.substr(begin, end - begin));
        }
        if (end == text.size()) {
            break;
        }
        begin = end + 1;
    }
    return parts;
}

bool white_space(std::uint32_t cp) noexcept {
    return (cp >= 9U && cp <= 13U) || cp == 0x20U || cp == 0x85U || cp == 0xa0U || cp == 0x1680U ||
           (cp >= 0x2000U && cp <= 0x200aU) || cp == 0x2028U || cp == 0x2029U || cp == 0x202fU ||
           cp == 0x205fU || cp == 0x3000U;
}

bool contains_space(std::string_view text) {
    auto it = text.begin();
    while (it != text.end()) {
        if (white_space(utf8::next(it, text.end()))) {
            return true;
        }
    }
    return false;
}

std::uint32_t frequency(std::string_view text, const char *context) {
    std::uint32_t value = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || value == 0 ||
        value > static_cast<std::uint32_t>(INT_MAX)) {
        reject(context, "frequency must be a positive decimal integer no greater than INT_MAX");
    }
    return value;
}

std::vector<cppjieba::DictUnit> parse_dictionary(std::string_view text, bool user) {
    const char *context = user ? "cppjieba.user_dictionary" : "cppjieba.dictionary";
    std::vector<cppjieba::DictUnit> units;
    std::set<std::string_view> seen;
    std::size_t begin = 0;
    while (begin < text.size()) {
        auto end = text.find('\n', begin);
        if (end == text.npos) {
            end = text.size();
        }
        auto line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        begin = end + 1;
        if (user && line.empty()) {
            continue;
        }
        const auto parts = split(line, ' ');
        if ((!user && parts.size() != 3) || (user && (parts.empty() || parts.size() > 3))) {
            reject(context, "dictionary line has an unsupported field count");
        }
        for (const auto part : parts) {
            if (contains_space(part)) {
                reject(context, "dictionary fields cannot contain whitespace");
            }
        }
        if (!seen.insert(parts[0]).second) {
            reject(context, "dictionary contains a duplicate word");
        }
        cppjieba::DictUnit unit;
        if (!cppjieba::DecodeUTF8RunesInString(parts[0].data(), parts[0].size(), unit.word) ||
            unit.word.empty() || unit.word.size() > 512) {
            reject(context, "dictionary word must contain 1 to 512 scalars");
        }
        unit.weight = parts.size() == 3 ? static_cast<double>(frequency(parts[1], context)) : 0.0;
        unit.tag = std::string(parts.size() >= 2 ? parts.back() : std::string_view{});
        units.push_back(std::move(unit));
    }
    if (!user && units.empty()) {
        reject(context, "main dictionary must not be empty");
    }
    return units;
}

double probability(std::string_view text) {
    double value = 0;
    const auto r =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || !std::isfinite(value) ||
        value > 0.0 || value < -3.14e100) {
        reject("cppjieba.hmm_model", "probability must be a finite number in [-3.14e100,0]");
    }
    return value;
}

std::unique_ptr<cppjieba::HMMModel> parse_model(std::string_view text) {
    std::vector<std::string_view> lines;
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        if (end == text.npos) {
            end = text.size();
        }
        const auto line = trim_ascii(text.substr(begin, end - begin));
        begin = end + 1;
        if (!line.empty() && line.front() != '#') {
            lines.push_back(line);
        }
    }
    if (lines.size() != 9) {
        reject("cppjieba.hmm_model", "HMM requires exactly nine data lines");
    }
    auto model = std::make_unique<cppjieba::HMMModel>();
    for (std::size_t i = 0; i < 5; ++i) {
        const auto fields = split(lines[i], ' ');
        if (fields.size() != 4) {
            reject("cppjieba.hmm_model", "HMM probability rows require four fields");
        }
        for (std::size_t j = 0; j < 4; ++j) {
            if (i == 0) {
                model->startProb[j] = probability(fields[j]);
            } else {
                model->transProb[i - 1][j] = probability(fields[j]);
            }
        }
    }
    for (std::size_t i = 0; i < 4; ++i) {
        auto &table = *model->emitProbVec[i];
        const auto entries = split(lines[i + 5], ',', true);
        if (entries.empty()) {
            reject("cppjieba.hmm_model", "HMM emission table must not be empty");
        }
        for (const auto entry : entries) {
            const auto colon = entry.find(':');
            if (colon == entry.npos || colon == 0 || colon + 1 == entry.size() ||
                entry.find(':', colon + 1) != entry.npos) {
                reject("cppjieba.hmm_model",
                       "HMM emission requires one scalar and one probability");
            }
            const auto key = entry.substr(0, colon);
            auto current = key.begin();
            const auto cp = utf8::next(current, key.end());
            if (current != key.end() ||
                !table.emplace(cp, probability(entry.substr(colon + 1))).second) {
                reject("cppjieba.hmm_model", "HMM emission keys must be unique single scalars");
            }
        }
    }
    return model;
}

bool ascii_word(std::uint32_t cp) noexcept {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9');
}
bool han(std::uint32_t cp) noexcept {
    return cp == 0x3007U || (cp >= 0x3400U && cp <= 0x4dbfU) || (cp >= 0x4e00U && cp <= 0x9fffU) ||
           (cp >= 0xf900U && cp <= 0xfaffU) || (cp >= 0x20000U && cp <= 0x2a6dfU) ||
           (cp >= 0x2a700U && cp <= 0x2b73fU) || (cp >= 0x2b740U && cp <= 0x2b81fU) ||
           (cp >= 0x2b820U && cp <= 0x2ceafU) || (cp >= 0x2ceb0U && cp <= 0x2ebefU) ||
           (cp >= 0x2ebf0U && cp <= 0x2ee5fU) || (cp >= 0x2f800U && cp <= 0x2fa1fU) ||
           (cp >= 0x30000U && cp <= 0x3134fU) || (cp >= 0x31350U && cp <= 0x323afU);
}

base::Result<TokenSequence> failed(std::string context, std::string message) {
    return base::Result<TokenSequence>::failure({std::move(message), std::move(context)});
}

} // namespace

struct CppJiebaTokenizer::Impl final {
    std::unique_ptr<cppjieba::DictTrie> dictionary;
    std::unique_ptr<cppjieba::HMMModel> model;
    std::unique_ptr<cppjieba::MixSegment> segment;
    bool use_hmm;
};

CppJiebaTokenizer::CppJiebaTokenizer(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
CppJiebaTokenizer::~CppJiebaTokenizer() noexcept = default;

base::Result<std::unique_ptr<CppJiebaTokenizer>> CppJiebaTokenizer::create(CppJiebaConfig config) {
    using Result = base::Result<std::unique_ptr<CppJiebaTokenizer>>;
    if (config.max_file_bytes == 0 || !valid_path(config.dictionary_path) ||
        !valid_path(config.hmm_model_path) ||
        (config.user_dictionary_path && !valid_path(*config.user_dictionary_path))) {
        return Result::failure(
            {"positive file budget and normalized absolute resource paths required",
             "cppjieba.config"});
    }
    try {
        const auto main_bytes =
            load_file(config.dictionary_path, config.max_file_bytes, "dictionary");
        auto main = parse_dictionary(main_bytes, false);
        const auto model_bytes =
            load_file(config.hmm_model_path, config.max_file_bytes, "hmm_model");
        auto model = parse_model(model_bytes);
        std::vector<cppjieba::DictUnit> user;
        if (config.user_dictionary_path) {
            const auto bytes =
                load_file(*config.user_dictionary_path, config.max_file_bytes, "user_dictionary");
            user = parse_dictionary(bytes, true);
        }
        auto impl = std::make_unique<Impl>();
        impl->dictionary = std::make_unique<cppjieba::DictTrie>(std::move(main), std::move(user));
        impl->model = std::move(model);
        impl->segment =
            std::make_unique<cppjieba::MixSegment>(impl->dictionary.get(), impl->model.get());
        impl->use_hmm = config.use_hmm;
        return Result::success(
            std::unique_ptr<CppJiebaTokenizer>(new CppJiebaTokenizer(std::move(impl))));
    } catch (InitFailure &failure) {
        return Result::failure(std::move(failure.error));
    }
}

base::Result<TokenSequence> CppJiebaTokenizer::tokenize(std::string_view input,
                                                        const TokenizationLimits &limits) const {
    if (limits.max_input_bytes == 0 || limits.max_tokens == 0 || limits.max_token_bytes == 0 ||
        limits.max_output_bytes == 0) {
        return failed("tokenizer.limits", "tokenization requires four positive limits");
    }
    const auto size = base::checked_narrow<std::uint64_t>(input.size(), "tokenizer.input_bytes");
    // 上游Rune索引为uint32且HMM反向循环用int；保守输入上限保证四状态临时索引可表示。
    if (!size || size.value() > limits.max_input_bytes ||
        size.value() > static_cast<std::uint64_t>(INT_MAX / 4)) {
        return failed("tokenizer.input_bytes", "input exceeds byte budget or cppjieba index range");
    }
    const auto invalid = utf8::find_invalid(input.begin(), input.end());
    if (invalid != input.end()) {
        return failed("tokenizer.utf8",
                      "invalid UTF-8 at byte " + std::to_string(invalid - input.begin()));
    }
    const auto nul = input.find('\0');
    if (nul != input.npos) {
        return failed("tokenizer.nul", "NUL at byte " + std::to_string(nul));
    }
    std::vector<std::string_view> spans;
    std::uint64_t total = 0;
    const char *error = nullptr;
    const auto accept = [&](std::string_view word) {
        if (word.size() > limits.max_token_bytes) {
            error = "tokenizer.token_bytes";
            return false;
        }
        if (spans.size() == limits.max_tokens) {
            error = "tokenizer.tokens";
            return false;
        }
        if (word.size() > limits.max_output_bytes - total) {
            error = "tokenizer.output_bytes";
            return false;
        }
        total += word.size();
        spans.push_back(word);
        return true;
    };
    auto current = input.begin();
    while (current != input.end()) {
        const auto start = current;
        const auto cp = utf8::next(current, input.end());
        const bool is_ascii = ascii_word(cp), is_han = han(cp);
        if (!is_ascii && !is_han) {
            continue;
        }
        while (current != input.end()) {
            auto next = current;
            const auto point = utf8::next(next, input.end());
            if ((is_ascii && !ascii_word(point)) || (is_han && !han(point))) {
                break;
            }
            current = next;
        }
        const std::string_view run{start, static_cast<std::size_t>(current - start)};
        if (is_ascii) {
            if (!accept(run)) {
                break;
            }
            continue;
        }
        cppjieba::RuneStrArray runes;
        if (!cppjieba::DecodeUTF8RunesInString(run.data(), run.size(), runes)) {
            throw std::runtime_error("cppjieba cannot decode validated run");
        }
        std::vector<cppjieba::WordRange> words;
        implementation_->segment->Cut(runes.begin(), runes.end(), words, implementation_->use_hmm);
        std::size_t offset = 0;
        for (const auto &word : words) {
            const auto begin = static_cast<std::size_t>(word.left->offset);
            const auto end = static_cast<std::size_t>(word.right->offset) + word.right->len;
            if (begin != offset || end <= begin || end > run.size()) {
                throw std::runtime_error("cppjieba violated contiguous word coverage");
            }
            offset = end;
            if (!accept(run.substr(begin, end - begin))) {
                break;
            }
        }
        if (error) {
            break;
        }
        if (offset != run.size()) {
            throw std::runtime_error("cppjieba omitted validated run bytes");
        }
    }
    if (error) {
        return failed(error, "token sequence exceeds limit");
    }
    TokenSequence output;
    output.reserve(spans.size());
    for (const auto word : spans) {
        output.emplace_back(word);
    }
    return base::Result<TokenSequence>::success(std::move(output));
}

} // namespace siftwing::text
