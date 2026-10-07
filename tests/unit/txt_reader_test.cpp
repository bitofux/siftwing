/*
 * PROJECT : SIFTWING
 * FILE    : txt_reader_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 用原创字面预期验证TXT整篇、Unicode、标题、限额、拥有性和批量控制
 * TEST CONTRACT :
 * -- 输入与预期不由被测规范化函数生成，UTF-8边界使用手写字节序列
 * -- 使用运行期检查并返回失败退出码，Release/NDEBUG下仍验证合同
 */

#include "siftwing/document/txt_reader.h"

#include <iostream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace siftwing::document;

std::size_t checks = 0;
std::size_t failures = 0;
const ReadLimits generous{4096, 4096, 32, 8192};

void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

siftwing::base::DocumentId id(std::uint64_t value) {
    return siftwing::base::DocumentId::from_integer(value).value();
}

std::string bytes(std::initializer_list<unsigned> values) {
    std::string result;
    for (const auto value : values) { result.push_back(static_cast<char>(value)); }
    return result;
}

ReadReport run(std::string path, std::string_view text, std::optional<std::string> title = "T",
               ReadLimits limits = generous) {
    TxtDocumentReader reader({{path, {id(7), std::move(title)}}});
    auto result = read_documents({{SourceKind::txt, std::move(path), text}}, reader, limits, {});
    if (!result) { throw std::runtime_error(result.error().message); }
    return std::move(result).value();
}

void rejected(const ReadReport& report, ReadIssueCode code, const char* label) {
    const auto& item = report.items.at(0);
    check(report.items.size() == 1 && report.counts.rejected == 1 && !item.record &&
          item.status == ReadStatus::rejected && item.issues.at(0).code == code && report.total_text_bytes == 0, label);
}

static_assert(std::is_base_of_v<DocumentReader, TxtDocumentReader>);
static_assert(!std::is_copy_constructible_v<TxtDocumentReader>);
static_assert(std::has_virtual_destructor_v<TxtDocumentReader>);

void whole_text_and_newlines() {
    const std::string input = "标题：山间\r\n作者：林青\r出处：原创\n正文：\n第1章 42 Apples!\n正文：这是文中的标记。";
    const std::string expected = "标题：山间\n作者：林青\n出处：原创\n正文：\n第1章 42 Apples!\n正文：这是文中的标记。";
    const auto report = run("books/guide.txt", input, "手册");
    const auto& record = *report.items.at(0).record;
    check(report.items.size() == 1 && report.counts.success == 1 && !report.stop, "one file remains one complete document");
    check(record.content == expected && record.title == "手册", "author, source, repeated markers, chapters, digits and punctuation are preserved");
    check(record.source.source_id.input_path == "books/guide.txt" && record.source.source_id.record_ordinal == 0 &&
          record.doc_id.value() == 7 && record.source.kind == SourceKind::txt, "source and explicit ID are preserved");
    check(!record.source.author && !record.source.url && !record.source.published_at_raw, "TXT body markers are not parsed into metadata");
    check(input.find('\r') != std::string::npos, "normalization does not write into input bytes");
    const std::vector<std::pair<std::string, std::string>> cases{
        {"a\nb\n", "a\nb\n"}, {"a\r\nb\r\n", "a\nb\n"}, {"a\rb\r", "a\nb\n"},
        {"a\r\r\nb", "a\n\nb"}, {"a\n\rb", "a\n\nb"}, {"a", "a"}, {" a\t\n", " a\t\n"}};
    for (const auto& [original, wanted] : cases) {
        const auto value = run("a.txt", original);
        check(value.items.at(0).record->content == wanted, "hand-written newline and whitespace-preservation cases");
        check(value.total_text_bytes == wanted.size() + 1, "final text count includes title and normalized bytes");
    }
    const std::string bom = bytes({0xef, 0xbb, 0xbf});
    const auto stripped = run("a.txt", bom + "甲\r\n乙", "札记", {11, 13, 1, 13});
    check(stripped.items.at(0).record->content == "甲\n乙" && stripped.total_text_bytes == 13,
          "input eleven bytes becomes seven-byte body plus six-byte title at inclusive limit");
    const auto interior = run("a.txt", bom + "a" + bom + "b");
    check(interior.items.at(0).record->content == "a" + bom + "b", "only the initial BOM is removed");
    const auto repeated_bom = run("a.txt", bom + bom);
    check(repeated_bom.items.at(0).record->content == bom, "second BOM is preserved and is not White_Space");
}

void empty_and_unicode() {
    const std::vector<std::string> empty_cases{"", " \t\n\r\v\f", bytes({0xef, 0xbb, 0xbf}),
        "\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000"};
    for (const auto& text : empty_cases) {
        const auto report = run("empty.txt", text);
        check(report.counts.no_text == 1 && !report.items.at(0).record && !report.stop &&
              report.items.at(0).issues.at(0).code == ReadIssueCode::empty_content && report.total_text_bytes == 0,
              "empty, BOM-only and Unicode White_Space inputs have no document");
    }
    // Unicode标量边界、非字符与零宽字符合法；这些输入不能被ASCII或空白策略误丢弃。
    const std::vector<std::string> valid_cases{bytes({0x01}), bytes({0x7f}), bytes({0xc2,0x80}), bytes({0xdf,0xbf}),
        bytes({0xe0,0xa0,0x80}), bytes({0xed,0x9f,0xbf}), bytes({0xee,0x80,0x80}), bytes({0xef,0xbf,0xbf}),
        bytes({0xf0,0x90,0x80,0x80}), bytes({0xf4,0x8f,0xbf,0xbf}), "中文 English 🙂", "\u200b", "\u180e"};
    for (const auto& text : valid_cases) {
        const auto report = run("a.txt", text);
        check(report.counts.success == 1 && report.items.at(0).record->content == text,
              "valid Unicode scalar boundaries and non-whitespace characters are preserved");
    }
    const std::vector<std::string> invalid_cases{bytes({0x80}), bytes({0xbf}), bytes({0xc0,0xaf}), bytes({0xc1,0x81}),
        bytes({0xc2}), bytes({0xc2,0x20}), bytes({0xe0,0x80,0xaf}), bytes({0xe1,0x80}), bytes({0xe1,0x20,0x80}),
        bytes({0xed,0xa0,0x80}), bytes({0xed,0xbf,0xbf}), bytes({0xf0,0x80,0x80,0xaf}), bytes({0xf1,0x80,0x80}),
        bytes({0xf4,0x90,0x80,0x80}), bytes({0xf5,0x80,0x80,0x80}), bytes({0xf8,0x88,0x80,0x80,0x80}),
        bytes({0xfc,0x84,0x80,0x80,0x80,0x80}), bytes({0xfe}), bytes({0xff}), bytes({0xff,0xfe,0x41,0x00})};
    for (const auto& text : invalid_cases) {
        const auto report = run("a.txt", "A" + text);
        rejected(report, ReadIssueCode::unsupported_encoding, "truncated, overlong, surrogate, out-of-range and non-UTF8 bytes reject");
        check(report.items.at(0).issues.at(0).region == "byte_offset: 1", "invalid UTF-8 diagnostic uses original byte offset");
    }
    const auto after_bom = run("a.txt", bytes({0xef,0xbb,0xbf,0xe2,0x82}));
    check(after_bom.items.at(0).issues.at(0).region == "byte_offset: 3", "offset is not rebased after removing BOM");
    const auto nul = run("a.txt", std::string{"a\0b", 3});
    rejected(nul, ReadIssueCode::malformed_input, "NUL is rejected as text content even though UTF-8 permits its encoding");
    check(nul.items.at(0).issues.at(0).region == "byte_offset: 1", "NUL rejection is located");
}

void titles_and_limits() {
    const std::vector<std::pair<std::string, std::string>> names{
        {"dir/文章.txt", "文章"}, {"dir/archive.part.txt", "archive.part"}, {"dir/README", "README"},
        {"dir/.txt", ".txt"}, {"dir/.hidden.txt", ".hidden"}, {"dir/name.", "name"}};
    for (const auto& [path, wanted] : names) {
        const auto report = run(path, "body", std::nullopt);
        check(report.counts.warning == 1 && report.items.at(0).record->title == wanted &&
              report.items.at(0).issues.at(0).code == ReadIssueCode::title_fallback && !report.stop,
              "fallback uses basename without final extension and retains warning");
    }
    for (const auto& title : std::vector<std::string>{"", " \t", "\u3000"}) {
        const auto report = run("a.txt", "body", title);
        check(report.items.at(0).record->title == "a" && report.counts.warning == 1, "explicit blank title falls back");
    }
    const auto explicit_title = run("dir/other.txt", "body", "  指定\r\n标题  ");
    check(explicit_title.items.at(0).record->title == "  指定\r\n标题  " && explicit_title.counts.success == 1,
          "nonblank explicit title is preserved, including whitespace and newline");
    rejected(run("a.txt", "body", bytes({0xed,0xa0,0x80})), ReadIssueCode::malformed_input, "invalid title rejects document");
    rejected(run("a.txt", "body", std::string{"a\0b",3}), ReadIssueCode::malformed_input, "NUL title rejects document");
    rejected(run(bytes({0xff}) + ".txt", "body", std::nullopt), ReadIssueCode::malformed_input, "invalid UTF-8 fallback filename rejects title");
    const auto supplied = run(bytes({0xff}) + ".txt", "body", "title");
    check(supplied.counts.success == 1, "logical source bytes need not be UTF-8 when supplied title is valid");
    rejected(run(" .txt", "body", std::nullopt), ReadIssueCode::malformed_input, "blank fallback is not silently delivered");
    rejected(run("a.txt", "body", "T", {3,5,1,5}), ReadIssueCode::input_too_large, "input limit rejects before parsing");
    rejected(run("a.txt", "body", "T", {4,4,1,5}), ReadIssueCode::document_too_large, "title participates in single-document limit");
    const auto exact = run("a.txt", "body", "T", {4,5,1,5});
    check(exact.counts.success == 1 && exact.total_text_bytes == 5, "exact input/document/total bounds are inclusive");
    rejected(run("a.txt", bytes({0xef,0xbb,0xbf}) + "a", "T", {3,2,1,2}), ReadIssueCode::input_too_large, "input budget includes BOM");
    const auto shrunk = run("a.txt", "a\r\nb", "T", {4,4,1,4});
    check(shrunk.counts.success == 1 && shrunk.total_text_bytes == 4, "document limit uses normalized size, not original CRLF size");
    rejected(run("a.txt", "a\r\nb", "T", {4,3,1,4}), ReadIssueCode::document_too_large, "normalization does not permit one-byte over document limit");
    const auto batch_budget = run("a.txt", "body", "T", {4,5,1,4});
    check(batch_budget.items.empty() && batch_budget.stop && batch_budget.stop->reason == ReadStopReason::total_text_limit,
          "batch total limit stops without publishing a truncated or uncounted candidate");
    const auto huge = run("a.txt", std::string(2048, 'x'), "T", {2048,16,1,16});
    rejected(huge, ReadIssueCode::document_too_large, "large body is rejected whole at output preflight");
}

void repeated_batch_and_ownership() {
    std::map<std::string, TxtDocumentMetadata> metadata{{"a.txt", {id(0), "甲"}},
                                                     {"b.txt", {id(std::numeric_limits<std::uint64_t>::max()), "乙"}}};
    TxtDocumentReader reader(metadata);
    metadata.at("a.txt").title = "changed";
    const std::vector<ReaderInput> inputs{{SourceKind::txt, "b.txt", "second"}, {SourceKind::txt, "a.txt", "first"}};
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        auto result = read_documents(inputs, reader, generous, {});
        if (!result) { throw std::runtime_error(result.error().message); }
        const auto report = std::move(result).value();
        check(report.items.at(0).record->doc_id.value() == 0 &&
              report.items.at(1).record->doc_id.value() == std::numeric_limits<std::uint64_t>::max(),
              "sorted batch and repeated reads preserve caller IDs including full value range");
        check(report.items.at(0).record->title == "甲" && report.items.at(1).record->content == "second",
              "adapter owns configuration independently of caller mutations");
    }
    TxtDocumentReader duplicate({{"a.txt", {id(1), "A"}}, {"b.txt", {id(1), "B"}}});
    check(!read_documents(inputs, duplicate, generous, {}), "common collector rejects duplicate delivered IDs across TXT files");
    const std::string bad = bytes({0xff});
    const std::vector<ReaderInput> mixed{{SourceKind::txt, "a.txt", bad}, {SourceKind::txt, "b.txt", "body"}};
    auto stopped = read_documents(mixed, reader, generous, {});
    check(stopped && stopped.value().counts.rejected == 1 && stopped.value().items.size() == 1 && stopped.value().stop,
          "default failure policy stops after malformed TXT");
    auto continued = read_documents(mixed, reader, generous, {FailurePolicy::continue_reading, PartialPolicy::reject});
    check(continued && continued.value().counts.rejected == 1 && continued.value().counts.success == 1 && !continued.value().stop,
          "explicit continuation keeps rejection and following valid TXT");
    auto count_limited = read_documents(inputs, reader, {4096,4096,1,8192}, {});
    check(count_limited && count_limited.value().items.size() == 1 && count_limited.value().stop &&
          count_limited.value().stop->reason == ReadStopReason::record_limit,
          "TXT integration respects common record budget before next file");
    auto unsupported = read_documents({{SourceKind::rss, "other.xml", "<rss/>"}}, reader, generous, {});
    check(unsupported && unsupported.value().items.at(0).issues.at(0).code == ReadIssueCode::unsupported_format,
          "known non-TXT kind reports unsupported format without consulting TXT metadata");
    bool absent = false;
    try { static_cast<void>(read_documents({{SourceKind::txt, "unconfigured.txt", "body"}}, reader, generous, {})); }
    catch (const std::out_of_range&) { absent = true; }
    check(absent, "missing ID/title configuration is a caller exception, not a fabricated input failure");
}

ReadReport escaped_txt_report() {
    std::string original = "作者：原创\r\n正文：纸船。";
    const auto original_copy = original;
    auto report = run("a.txt", original, "纸船");
    check(original == original_copy, "caller input bytes remain unchanged");
    original.assign(original.size(), 'x');
    return report; // 输入及局部适配器/配置销毁，正文仍须由报告拥有。
}

class StoppingSink final : public ReadSink {
public:
    std::size_t calls = 0;
    bool accept(ReadItem) override { ++calls; return false; }
};

void ownership_and_direct_stop() {
    const auto report = escaped_txt_report();
    check(report.items.at(0).record->content == "作者：原创\n正文：纸船。" && report.items.at(0).record->title == "纸船",
          "report owns normalized content/title after source and configuration destruction");
    TxtDocumentReader reader({{"a.txt", {id(1), "A"}}});
    StoppingSink sink;
    reader.read({SourceKind::txt, "a.txt", "first\nsecond\nthird"}, generous, sink);
    check(sink.calls == 1, "direct sink false ends the single-document adapter immediately");
    for (unsigned field = 0; field < 4; ++field) {
        auto zero = generous;
        switch (field) {
        case 0: zero.max_input_bytes = 0; break;
        case 1: zero.max_document_bytes = 0; break;
        case 2: zero.max_records = 0; break;
        default: zero.max_total_text_bytes = 0; break;
        }
        bool rejected_config = false;
        try { reader.read({SourceKind::txt, "a.txt", "body"}, zero, sink); }
        catch (const std::invalid_argument&) { rejected_config = true; }
        check(rejected_config && sink.calls == 1, "direct read refuses zero limits without emitting an outcome");
    }
}

} // namespace

int main() {
    try {
        whole_text_and_newlines(); empty_and_unicode(); titles_and_limits();
        repeated_batch_and_ownership(); ownership_and_direct_stop();
    } catch (const std::exception& error) {
        ++failures; std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    }
    std::cout << "TXT reader checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
