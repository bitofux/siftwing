/*
 * PROJECT : SIFTWING
 * FILE    : txt_reader.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 严格验证TXT字节，预计算规范化大小后交付拥有正文的单篇结果
 * IMPLEMENTATION :
 * -- UTF-8先由utfcpp验证，再按码点判断空白，避免字节误判或替换非法编码
 * -- 规范化只删除开头BOM与CRLF中的CR，不修改作者/章节等文字
 * -- 使用借用视图预检标题和最终大小，正文只在预算通过后分配
 */

#include "siftwing/document/txt_reader.h"

#include "siftwing/base/checked.h"

#include <utf8.h>

#include <iterator>
#include <stdexcept>
#include <utility>

namespace siftwing::document {
namespace {

// Unicode15.1 White_Space属性；仅用于判空，不用于裁剪、分词或全文规范化。
bool white_space(std::uint32_t point) noexcept {
    return (point >= 0x0009U && point <= 0x000dU) || point == 0x0020U || point == 0x0085U ||
           point == 0x00a0U || point == 0x1680U || (point >= 0x2000U && point <= 0x200aU) ||
           point == 0x2028U || point == 0x2029U || point == 0x202fU || point == 0x205fU || point == 0x3000U;
}

bool only_white_space(std::string_view valid_text) {
    auto position = valid_text.begin();
    while (position != valid_text.end()) {
        if (!white_space(utf8::next(position, valid_text.end()))) { return false; }
    }
    return true;
}

std::string_view fallback_title(std::string_view path) noexcept {
    const auto slash = path.rfind('/');
    auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.rfind('.');
    if (dot != std::string_view::npos && dot != 0) { name = name.substr(0, dot); }
    return name;
}

std::size_t normalized_size(std::string_view body) noexcept {
    auto size = body.size();
    for (std::size_t index = 0; index < body.size(); ++index) {
        if (body[index] == '\r' && index + 1 < body.size() && body[index + 1] == '\n') {
            --size;
            ++index;
        }
    }
    return size;
}

std::string normalized_body(std::string_view body, std::size_t size) {
    std::string result;
    result.reserve(size);
    for (std::size_t index = 0; index < body.size(); ++index) {
        if (body[index] == '\r') {
            result.push_back('\n');
            if (index + 1 < body.size() && body[index + 1] == '\n') { ++index; }
        } else {
            result.push_back(body[index]);
        }
    }
    return result;
}

} // namespace

TxtDocumentReader::TxtDocumentReader(std::map<std::string, TxtDocumentMetadata> metadata)
    : metadata_(std::move(metadata)) {}

void TxtDocumentReader::read(const ReaderInput& input, const ReadLimits& limits, ReadSink& sink) {
    if (limits.max_input_bytes == 0 || limits.max_document_bytes == 0 ||
        limits.max_records == 0 || limits.max_total_text_bytes == 0) {
        throw std::invalid_argument("TXT read requires positive limits");
    }
    const auto outcome = [&](ReadStatus status, ReadIssueCode code, std::string message, std::string region = "") {
        static_cast<void>(sink.emit({{input.input_path, 0}, status, std::nullopt,
                                     {{code, std::move(message), std::move(region)}}}));
    };
    if (input.kind != SourceKind::txt) {
        outcome(ReadStatus::rejected, ReadIssueCode::unsupported_format, "TXT reader accepts only TXT inputs");
        return;
    }
    auto input_size = base::checked_narrow<std::uint64_t>(input.bytes.size(), "txt.input_bytes");
    if (!input_size || input_size.value() > limits.max_input_bytes) {
        outcome(ReadStatus::rejected, ReadIssueCode::input_too_large, "TXT input exceeds byte limit");
        return;
    }
    const auto& metadata = metadata_.at(input.input_path); // 缺失配置是调用方违约，异常不伪装成普通输入失败。
    const auto invalid = utf8::find_invalid(input.bytes.begin(), input.bytes.end());
    if (invalid != input.bytes.end()) {
        outcome(ReadStatus::rejected, ReadIssueCode::unsupported_encoding, "TXT input is not valid UTF-8",
                "byte_offset: " + std::to_string(std::distance(input.bytes.begin(), invalid)));
        return;
    }
    const auto nul = input.bytes.find('\0');
    if (nul != std::string_view::npos) {
        outcome(ReadStatus::rejected, ReadIssueCode::malformed_input, "TXT input contains NUL",
                "byte_offset: " + std::to_string(nul));
        return;
    }
    auto body = input.bytes;
    if (body.size() >= 3 && body.substr(0, 3) == "\xef\xbb\xbf") { body.remove_prefix(3); }
    if (only_white_space(body)) {
        outcome(ReadStatus::no_text, ReadIssueCode::empty_content, "TXT input has no non-whitespace content");
        return;
    }
    auto title = metadata.title ? std::string_view{*metadata.title} : std::string_view{};
    if (utf8::find_invalid(title.begin(), title.end()) != title.end() || title.find('\0') != std::string_view::npos) {
        outcome(ReadStatus::rejected, ReadIssueCode::malformed_input, "TXT title is not valid NUL-free UTF-8", "metadata:title");
        return;
    }
    const bool use_fallback = only_white_space(title);
    if (use_fallback) {
        title = fallback_title(input.input_path);
        if (utf8::find_invalid(title.begin(), title.end()) != title.end() || only_white_space(title)) {
            outcome(ReadStatus::rejected, ReadIssueCode::malformed_input, "TXT fallback title is invalid or blank", "metadata:title");
            return;
        }
    }
    const auto body_size = normalized_size(body);
    auto title_bytes = base::checked_narrow<std::uint64_t>(title.size(), "txt.title_bytes");
    auto body_bytes = base::checked_narrow<std::uint64_t>(body_size, "txt.body_bytes");
    if (!title_bytes || !body_bytes) {
        outcome(ReadStatus::rejected, ReadIssueCode::document_too_large, "TXT document size is not representable");
        return;
    }
    auto total = base::checked_add(title_bytes.value(), body_bytes.value(), "txt.document_bytes");
    if (!total || total.value() > limits.max_document_bytes) {
        outcome(ReadStatus::rejected, ReadIssueCode::document_too_large, "TXT title plus content exceeds document byte limit");
        return;
    }
    // 输出长度与预算已经确定才构造正文；正文与标题都复制到拥有型结果，不借用输入/配置。
    SourceIdentity source{input.input_path, 0};
    DocumentRecord record{metadata.doc_id, std::string{title}, normalized_body(body, body_size),
                          {SourceKind::txt, source, {}, {}, {}}};
    ReadItem item{source, use_fallback ? ReadStatus::warning : ReadStatus::success, std::move(record), {}};
    if (use_fallback) {
        item.issues.push_back({ReadIssueCode::title_fallback, "TXT title uses input filename without extension", "metadata:title"});
    }
    static_cast<void>(sink.emit(std::move(item))); // 单篇已结束；false后不会再调用sink或处理其他记录。
}

} // namespace siftwing::document
