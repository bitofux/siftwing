/*
 * PROJECT : SIFTWING
 * FILE    : txt_corpus_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-08
 * BRIEF   : 专用TXT文件接入验收探针，显式选择输入并输出拥有型报告
 * TEST CONTRACT :
 * -- Linux逐组件打开且不跟随链接，普通文件大小在缓冲区分配前检查
 * -- 文件适配层只装载当前文件，复用既有TXT字节合同，不建立生产加载API
 * -- stdout验收JSON以hex保存原始字符串字节，报告成立不表示每篇成功
 */

#include "siftwing/document/txt_reader.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace siftwing::document;

// 同步、独占fd；只读close的错误不改变已交付文本，不在析构中抛异常或重试close。
class FileDescriptor final {
public:
    explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
    ~FileDescriptor() { if (value_ >= 0) { static_cast<void>(::close(value_)); } }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const noexcept { return value_; }
    void reset(int value) noexcept {
        if (value_ >= 0) { static_cast<void>(::close(value_)); }
        value_ = value;
    }
private:
    int value_;
};

std::vector<std::string> components(std::string_view path, bool absolute) {
    if (path.empty() || (path.front() == '/') != absolute || path.find('\0') != path.npos) {
        throw std::invalid_argument("invalid absolute root or relative input path");
    }
    std::vector<std::string> parts;
    std::size_t begin = absolute ? 1 : 0;
    if (absolute && path == "/") { return parts; }
    while (begin < path.size()) {
        const auto slash = path.find('/', begin);
        const auto part = path.substr(begin, slash == path.npos ? path.npos : slash - begin);
        if (part.empty() || part == "." || part == "..") {
            throw std::invalid_argument("empty, dot or parent path component");
        }
        parts.emplace_back(part);
        if (slash == path.npos) { return parts; }
        begin = slash + 1;
    }
    throw std::invalid_argument("trailing path separator");
}

std::string io_message(const char* operation, int error) {
    return std::string{operation} + ": " + std::strerror(error);
}

int open_directory(int parent, const std::string& part) {
    return ::openat(parent, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

std::uint64_t positive_number(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
        throw std::invalid_argument("limits must be positive decimal uint64 values");
    }
    return value;
}

ssize_t read_retry(int fd, void* buffer, std::size_t size) {
    ssize_t count;
    do { count = ::read(fd, buffer, size); } while (count < 0 && errno == EINTR);
    return count;
}

struct LoadedInput final {
    std::string bytes;
    std::optional<ReadIssue> issue;
    ReadStatus status = ReadStatus::failed;
};

LoadedInput load_file(int root, const std::string& path, std::uint64_t limit) {
    const auto parts = components(path, false);
    FileDescriptor directory;
    int parent = root; // 借用调用方固定根fd，逐组件fd钉住对象而非重新拼接绝对路径。
    const auto failed = [](std::string message) {
        return LoadedInput{"", ReadIssue{ReadIssueCode::io_error, std::move(message), "file"}, ReadStatus::failed};
    };
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        const int next = open_directory(parent, parts[index]);
        if (next < 0) { return failed(io_message("open input directory", errno)); }
        directory.reset(next);
        parent = directory.get();
    }
    // NONBLOCK避免误选FIFO时在fstat之前阻塞；随后只接纳普通文件。
    FileDescriptor file{::openat(parent, parts.back().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
    if (file.get() < 0) { return failed(io_message("open input file", errno)); }
    struct stat initial{};
    if (::fstat(file.get(), &initial) != 0) { return failed(io_message("stat input file", errno)); }
    if (!S_ISREG(initial.st_mode)) { return failed("input is not a regular file"); }
    auto size = siftwing::base::checked_narrow<std::uint64_t>(initial.st_size, "probe.file_bytes");
    if (!size || size.value() > limit) {
        return {"", ReadIssue{ReadIssueCode::input_too_large, "file exceeds input byte limit before allocation", "file"},
                ReadStatus::rejected};
    }
    auto native_size = siftwing::base::checked_narrow<std::size_t>(size.value(), "probe.buffer_bytes");
    LoadedInput result;
    if (!native_size || native_size.value() > result.bytes.max_size()) {
        return {"", ReadIssue{ReadIssueCode::input_too_large, "file size cannot fit a string", "file"}, ReadStatus::rejected};
    }
    result.bytes.reserve(native_size.value());
    std::array<char, 65536> buffer{};
    while (result.bytes.size() < native_size.value()) {
        const auto wanted = std::min(buffer.size(), native_size.value() - result.bytes.size());
        const auto count = read_retry(file.get(), buffer.data(), wanted);
        if (count < 0) { return failed(io_message("read input file", errno)); }
        if (count == 0) { return failed("input shortened during read"); }
        result.bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    // 读取额外一个字节检测增长，不为增长分配；再核对同一fd的大小与变动时点。
    char extra = 0;
    const auto tail = read_retry(file.get(), &extra, 1);
    if (tail < 0) { return failed(io_message("read input tail", errno)); }
    if (tail != 0) { return failed("input grew during read"); }
    struct stat final_state{};
    if (::fstat(file.get(), &final_state) != 0) { return failed(io_message("stat input after read", errno)); }
    if (initial.st_size != final_state.st_size || initial.st_mtim.tv_sec != final_state.st_mtim.tv_sec ||
        initial.st_mtim.tv_nsec != final_state.st_mtim.tv_nsec || initial.st_ctim.tv_sec != final_state.st_ctim.tv_sec ||
        initial.st_ctim.tv_nsec != final_state.st_ctim.tv_nsec) {
        return failed("input changed during read");
    }
    return result;
}

class FileReader final : public DocumentReader {
public:
    FileReader(int root, std::map<std::string, TxtDocumentMetadata> metadata)
        : root_(root), txt_(std::move(metadata)) {}
    void read(const ReaderInput& request, const ReadLimits& limits, ReadSink& sink) override {
        auto loaded = load_file(root_, request.input_path, limits.max_input_bytes);
        if (loaded.issue) {
            static_cast<void>(sink.emit({{request.input_path, 0}, loaded.status, std::nullopt, {std::move(*loaded.issue)}}));
            return;
        }
        txt_.read({SourceKind::txt, request.input_path, loaded.bytes}, limits, sink);
        // loaded在本次调用结束销毁；接收端已拥有文档，批量停止前不会预装载下一文件。
    }
private:
    int root_; // 同步借用main的根fd，不负责close，不逃逸到返回报告。
    TxtDocumentReader txt_;
};

void hex_string(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::cout.put('"');
    for (const char value : text) {
        const auto byte = static_cast<unsigned char>(value);
        std::cout.put(digits[byte >> 4U]);
        std::cout.put(digits[byte & 15U]);
    }
    std::cout.put('"');
}

const char* status_name(ReadStatus status) {
    switch (status) {
    case ReadStatus::success: return "success";
    case ReadStatus::warning: return "warning";
    case ReadStatus::no_text: return "no_text";
    case ReadStatus::rejected: return "rejected";
    case ReadStatus::failed: return "failed";
    }
    throw std::logic_error("unknown report status");
}

void write_report(const ReadReport& report) {
    std::cout << "{\"schema_version\":1,\"total_text_bytes\":" << report.total_text_bytes
              << ",\"counts\":{\"success\":" << report.counts.success << ",\"warning\":" << report.counts.warning
              << ",\"no_text\":" << report.counts.no_text << ",\"rejected\":" << report.counts.rejected
              << ",\"failed\":" << report.counts.failed << "},\"items\":[";
    bool first = true;
    for (const auto& item : report.items) {
        if (!first) { std::cout.put(','); }
        first = false;
        std::cout << "{\"path_hex\":"; hex_string(item.source_id.input_path);
        std::cout << ",\"ordinal\":" << item.source_id.record_ordinal << ",\"status\":\"" << status_name(item.status)
                  << "\",\"record\":";
        if (item.record) {
            std::cout << "{\"id\":" << item.record->doc_id.value() << ",\"title_hex\":"; hex_string(item.record->title);
            std::cout << ",\"content_hex\":"; hex_string(item.record->content); std::cout.put('}');
        } else { std::cout << "null"; }
        std::cout << ",\"issues\":[";
        bool first_issue = true;
        for (const auto& issue : item.issues) {
            if (!first_issue) { std::cout.put(','); }
            first_issue = false;
            // 仅验收schema使用当前枚举数值，不承诺生产错误码或持久化兼容。
            std::cout << "{\"code\":" << static_cast<int>(issue.code) << ",\"message_hex\":"; hex_string(issue.message);
            std::cout << ",\"region_hex\":"; hex_string(issue.region); std::cout.put('}');
        }
        std::cout << "]}";
    }
    std::cout << "],\"stop\":";
    if (report.stop) {
        std::cout << "{\"reason\":" << static_cast<int>(report.stop->reason) << ",\"path_hex\":";
        hex_string(report.stop->source_id.input_path);
        std::cout << ",\"ordinal\":" << report.stop->source_id.record_ordinal << '}';
    } else { std::cout << "null"; }
    std::cout << "}\n";
}

int run(int argc, char** argv) {
    if (argc < 7) { throw std::invalid_argument("usage: probe ROOT INPUT_BYTES DOCUMENT_BYTES RECORDS TOTAL_BYTES PATH..."); }
    const auto root_parts = components(argv[1], true);
    const ReadLimits limits{positive_number(argv[2]), positive_number(argv[3]), positive_number(argv[4]), positive_number(argv[5])};
    std::vector<std::string> paths;
    for (int index = 6; index < argc; ++index) {
        static_cast<void>(components(argv[index], false));
        paths.emplace_back(argv[index]);
    }
    std::sort(paths.begin(), paths.end());
    if (std::adjacent_find(paths.begin(), paths.end()) != paths.end()) { throw std::invalid_argument("duplicate input path"); }
    FileDescriptor root{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (root.get() < 0) { throw std::invalid_argument(io_message("open root directory", errno)); }
    for (const auto& part : root_parts) {
        const int next = open_directory(root.get(), part);
        if (next < 0) { throw std::invalid_argument(io_message("open root component", errno)); }
        root.reset(next);
    }
    std::vector<ReaderInput> requests;
    std::map<std::string, TxtDocumentMetadata> metadata;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        // ID由这个调用方按显式请求排序分配，不改变TXT适配器配置合同或跨批身份语义。
        auto id = siftwing::base::DocumentId::from_integer(index);
        if (!id) { throw std::invalid_argument("too many input identities"); }
        metadata.emplace(paths[index], TxtDocumentMetadata{id.value(), std::nullopt});
        requests.push_back({SourceKind::txt, paths[index], {}}); // 测试文件层稍后按本项请求受控装载。
    }
    FileReader reader(root.get(), std::move(metadata));
    auto result = read_documents(requests, reader, limits, {FailurePolicy::continue_reading, PartialPolicy::reject});
    if (!result) { throw std::invalid_argument(result.error().message); }
    write_report(result.value());
    std::cout.flush();
    return std::cout ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
