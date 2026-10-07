/*
 * PROJECT : SIFTWING
 * FILE    : document_reader_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 验证读取结果、来源/身份、同步停止、确定顺序、部分策略与四项接纳上限
 */

#include "siftwing/document/document_reader.h"
#include "../fixtures/reader_contract/scenarios.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

using namespace siftwing::document;
using siftwing::test_fixture::complete;
using siftwing::test_fixture::diagnostic;
using siftwing::test_fixture::partial;

static_assert(std::is_abstract_v<DocumentReader> && std::has_virtual_destructor_v<DocumentReader>);
static_assert(std::is_abstract_v<ReadSink> && std::has_virtual_destructor_v<ReadSink>);
static_assert(!std::is_copy_constructible_v<DocumentReader>);
static_assert(std::is_copy_constructible_v<ReadReport> && std::is_move_constructible_v<ReadReport>);
static_assert(std::is_nothrow_destructible_v<ReadReport>);
static_assert(std::is_same_v<decltype(ReaderInput::bytes), std::string_view>);

std::size_t checks = 0;
std::size_t failures = 0;

void check(bool value, const char* description) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}

const ReadLimits limits{1024, 512, 32, 4096};
const ReadPolicy continuing{FailurePolicy::continue_reading, PartialPolicy::reject};

class ScriptReader final : public DocumentReader {
public:
    std::map<std::string, std::vector<ReadItem>> scripts;
    std::vector<std::string> visited;
    std::size_t delivered = 0;

    void read(const ReaderInput& input, const ReadLimits&, ReadSink& sink) override {
        visited.push_back(input.input_path);
        for (const auto& item : scripts.at(input.input_path)) {
            ++delivered;
            if (!sink.emit(item)) { return; }
        }
    }
};

class ActionReader final : public DocumentReader {
public:
    explicit ActionReader(std::function<void(const ReaderInput&, ReadSink&)> action)
        : action_(std::move(action)) {}
    void read(const ReaderInput& input, const ReadLimits&, ReadSink& sink) override { action_(input, sink); }
private:
    std::function<void(const ReaderInput&, ReadSink&)> action_;
};

ReadReport report_of(siftwing::base::Result<ReadReport> result) {
    if (!result) { throw std::runtime_error(result.error().message); }
    return std::move(result).value();
}

std::uint64_t count_sum(const ReadCounts& count) {
    return count.success + count.warning + count.no_text + count.rejected + count.failed;
}

void outcomes_and_order() {
    ScriptReader reader;
    auto warned = complete("a.txt", 1, 12, "灯", "正文");
    warned.status = ReadStatus::warning;
    warned.issues = {{ReadIssueCode::missing_metadata, "author absent", "author"}};
    reader.scripts["a.txt"] = {complete("a.txt", 0, 11, "A", "first"), warned,
        diagnostic("a.txt", 2, ReadStatus::no_text, ReadIssueCode::empty_content, "empty body")};
    reader.scripts["b.txt"] = {diagnostic("b.txt", 0, ReadStatus::rejected, ReadIssueCode::encrypted, "encrypted"),
        diagnostic("b.txt", 1, ReadStatus::failed, ReadIssueCode::malformed_input, "bad structure")};
    const std::vector<ReaderInput> inputs{{SourceKind::txt, "b.txt", "b"}, {SourceKind::txt, "a.txt", "a"}};
    auto report = report_of(read_documents(inputs, reader, limits, continuing));
    check(reader.visited == std::vector<std::string>{"a.txt", "b.txt"}, "files are sorted before adapter calls");
    check(inputs.front().input_path == "b.txt", "sorting does not modify caller inputs");
    check(report.items.size() == 5 && !report.stop, "all statuses retained when continuation is explicit");
    check(report.counts.success == 1 && report.counts.warning == 1 && report.counts.no_text == 1 &&
          report.counts.rejected == 1 && report.counts.failed == 1, "five counters independently reflect hand-written outcomes");
    check(count_sum(report.counts) == report.items.size(), "counter sum equals saved event count");
    check(report.total_text_bytes == 15, "byte count is A+first=6 and UTF-8 lamp+body=9, excludes metadata");
    check(report.items[1].record->title == "灯" && report.items[1].issues[0].region == "author",
          "warning retains full payload and precise diagnostic");
    check(!report.items[2].record && !report.items[3].record && !report.items[4].record,
          "no-text, rejection and failure do not publish document payloads");
    std::reverse(reader.visited.begin(), reader.visited.end());
    const auto repeat = report_of(read_documents(inputs, reader, limits, continuing));
    check(repeat.items[0].record->doc_id == report.items[0].record->doc_id && repeat.total_text_bytes == 15,
          "repeated scripted reads preserve deterministic identity and bytes");
    ScriptReader empty;
    const auto no_inputs = report_of(read_documents({}, empty, limits, {}));
    check(no_inputs.items.empty() && count_sum(no_inputs.counts) == 0 && !no_inputs.stop && empty.visited.empty(),
          "empty batch completes without invoking adapter");
}

void policy_and_partial() {
    ScriptReader reader;
    reader.scripts["a.txt"] = {complete("a.txt", 0, 1, "A", "body"),
        diagnostic("a.txt", 1, ReadStatus::failed, ReadIssueCode::io_error, "read failed"),
        complete("a.txt", 2, 2, "B", "unvisited")};
    reader.scripts["z.txt"] = {complete("z.txt", 0, 3, "Z", "unvisited")};
    const auto report = report_of(read_documents({{SourceKind::txt, "z.txt", "z"}, {SourceKind::txt, "a.txt", "a"}},
                                                 reader, limits, {}));
    check(report.items.size() == 2 && report.counts.failed == 1 && report.counts.success == 1,
          "failure policy keeps preceding success and triggering failure");
    check(reader.delivered == 2 && reader.visited == std::vector<std::string>{"a.txt"},
          "false stops adapter and prevents next file invocation");
    check(report.stop && report.stop->reason == ReadStopReason::failure_policy && report.stop->source_id.record_ordinal == 1,
          "failure stop identifies triggering logical record");
    ScriptReader partial_reader;
    partial_reader.scripts["articles/partial.txt"] = {partial()};
    const std::vector<ReaderInput> inputs{{SourceKind::txt, "articles/partial.txt", "raw"}};
    const auto rejected = report_of(read_documents(inputs, partial_reader, limits, {}));
    check(rejected.counts.rejected == 1 && !rejected.items[0].record && rejected.total_text_bytes == 0,
          "partial results are not implicitly accepted");
    check(rejected.items[0].issues.size() == 2 && rejected.items[0].issues[0].region == "paragraphs: 2" &&
          rejected.items[0].issues[1].code == ReadIssueCode::partial_not_allowed,
          "partial rejection preserves uncovered region and explicit policy reason");
    const auto accepted = report_of(read_documents(inputs, partial_reader, limits,
                                                 {FailurePolicy::stop, PartialPolicy::accept}));
    check(accepted.counts.warning == 1 && !accepted.stop && accepted.items[0].record->title == "纸船",
          "explicit partial acceptance preserves document and warning status");
    check(accepted.items[0].record->content == "河边有一只纸船。\n" && accepted.items[0].issues[0].region == "paragraphs: 2",
          "partial body and hand-written uncovered range are not rewritten");
    ScriptReader no_text;
    no_text.scripts["a.txt"] = {diagnostic("a.txt", 0, ReadStatus::no_text, ReadIssueCode::empty_content, "no text"),
                               complete("a.txt", 1, 1, "title", "body")};
    const auto continued = report_of(read_documents({{SourceKind::txt, "a.txt", ""}}, no_text, limits, {}));
    check(continued.items.size() == 2 && !continued.stop, "no-text alone does not trigger failure policy");
}

void input_and_document_limits() {
    ScriptReader reader;
    reader.scripts["a.txt"] = {complete("a.txt", 0, 1, "ab", "cde")};
    const auto equal = report_of(read_documents({{SourceKind::txt, "a.txt", "1234"}}, reader, {4, 5, 1, 5}, {}));
    check(equal.counts.success == 1 && equal.total_text_bytes == 5 && !equal.stop,
          "input/document/count/total boundaries all accept exact equality");
    reader.visited.clear();
    const auto oversized = report_of(read_documents({{SourceKind::txt, "a.txt", "12345"}}, reader, {4, 5, 1, 5}, {}));
    check(reader.visited.empty() && oversized.counts.rejected == 1, "input one byte over limit rejects before adapter call");
    check(oversized.items[0].issues[0].code == ReadIssueCode::input_too_large && oversized.stop,
          "input rejection has source and distinct reason");
    const auto too_long = report_of(read_documents({{SourceKind::txt, "a.txt", "1234"}}, reader, {4, 4, 2, 5}, continuing));
    check(too_long.counts.rejected == 1 && !too_long.items[0].record && too_long.total_text_bytes == 0 && !too_long.stop,
          "oversized document rejected whole and does not consume text budget");
    check(too_long.items[0].issues.back().code == ReadIssueCode::document_too_large,
          "document limit is independent of input limit");
    reader.scripts["a.txt"] = {complete("a.txt", 0, 1, "中", "文")};
    const auto chinese = report_of(read_documents({{SourceKind::txt, "a.txt", ""}}, reader, {4, 6, 1, 6}, {}));
    check(chinese.total_text_bytes == 6, "UTF-8 counts bytes rather than codepoints");
    reader.scripts["a.txt"] = {complete("a.txt", 0, 1, "a", "b"), complete("a.txt", 1, 2, "c", "d")};
    const auto total = report_of(read_documents({{SourceKind::txt, "a.txt", ""}}, reader, {4, 2, 5, 3}, continuing));
    check(total.items.size() == 1 && total.counts.success == 1 && total.total_text_bytes == 2,
          "total capacity stops before accepting candidate and preserves preceding record");
    check(total.stop && total.stop->reason == ReadStopReason::total_text_limit && total.stop->source_id.record_ordinal == 1,
          "total limit records omitted candidate location even with continue policy");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto wide = report_of(read_documents({{SourceKind::txt, "a.txt", ""}}, reader,
                                             {maximum, maximum, maximum, maximum}, continuing));
    check(wide.items.size() == 2 && wide.total_text_bytes == 4, "maximum configured limits do not narrow to signed integers");
}

void record_limits_and_failure_continuation() {
    ScriptReader reader;
    reader.scripts["a.txt"] = {
        diagnostic("a.txt", 0, ReadStatus::no_text, ReadIssueCode::empty_content, "none"),
        diagnostic("a.txt", 1, ReadStatus::failed, ReadIssueCode::io_error, "failed"),
        complete("a.txt", 2, 1, "a", "b"), complete("a.txt", 3, 2, "c", "d")};
    const auto report = report_of(read_documents({{SourceKind::txt, "a.txt", ""}}, reader, {4, 2, 2, 4}, continuing));
    check(report.items.size() == 2 && report.counts.no_text == 1 && report.counts.failed == 1,
          "non-document outcomes consume bounded result slots");
    check(report.stop && report.stop->reason == ReadStopReason::record_limit && report.stop->source_id.record_ordinal == 2,
          "within-file overflow candidate is identified without acceptance");
    check(reader.delivered == 3, "reader obeys stop and avoids later record");
    ScriptReader files;
    files.scripts["a.txt"] = {complete("a.txt", 0, 1, "a", "b")};
    files.scripts["b.txt"] = {complete("b.txt", 0, 2, "c", "d")};
    const auto before_next = report_of(read_documents({{SourceKind::txt, "b.txt", ""}, {SourceKind::txt, "a.txt", ""}},
                                                     files, {4, 2, 1, 4}, continuing));
    check(files.visited == std::vector<std::string>{"a.txt"} && before_next.stop->source_id.input_path == "b.txt",
          "known full record budget prevents next file invocation");
    ScriptReader over_input;
    over_input.scripts["b.txt"] = {complete("b.txt", 0, 2, "c", "d")};
    const auto continued = report_of(read_documents({{SourceKind::txt, "a.txt", "12345"}, {SourceKind::txt, "b.txt", "1234"}},
                                                   over_input, {4, 2, 2, 4}, continuing));
    check(continued.counts.rejected == 1 && continued.counts.success == 1 && !continued.stop,
          "continue policy permits next input after preflight input rejection");
}

void request_validation() {
    ScriptReader reader;
    auto invalid = limits;
    for (unsigned field = 0; field != 4; ++field) {
        invalid = limits;
        switch (field) {
        case 0: invalid.max_input_bytes = 0; break;
        case 1: invalid.max_document_bytes = 0; break;
        case 2: invalid.max_records = 0; break;
        default: invalid.max_total_text_bytes = 0; break;
        }
        check(!read_documents({}, reader, invalid, {}), "each zero limit is an explicit configuration error");
    }
    const std::vector<std::string> bad_paths{"", "/a", ".", "..", "a/../b", "a/./b", "a//b", "a/", std::string{"a\0b", 3}};
    for (const auto& path : bad_paths) {
        check(!read_documents({{SourceKind::txt, path, ""}}, reader, limits, {}), "invalid logical paths fail before reading");
    }
    check(!read_documents({{static_cast<SourceKind>(99), "a.txt", ""}}, reader, limits, {}), "unknown source kind rejects request");
    check(!read_documents({{SourceKind::txt, "a.txt", ""}, {SourceKind::rss, "a.txt", ""}}, reader, limits, {}),
          "duplicate input names reject before reading even across kinds");
    check(!read_documents({}, reader, limits, {static_cast<FailurePolicy>(99), PartialPolicy::reject}), "invalid failure policy rejects");
    check(!read_documents({}, reader, limits, {FailurePolicy::stop, static_cast<PartialPolicy>(99)}), "invalid partial policy rejects");
    check(reader.visited.empty(), "all invalid requests are side-effect free with respect to adapter");
}

// 三种来源共用同一接收合同；遍历6个文件顺序排列，以人工固定的身份/载荷证明输出确定。
void multi_kind_and_cross_input_ids() {
    ScriptReader reader;
    reader.scripts["a.txt"] = {complete("a.txt", 0, 17, "T", "aaa", SourceKind::txt)};
    reader.scripts["b.xml"] = {complete("b.xml", 0, 18, "R", "bbb", SourceKind::rss)};
    reader.scripts["c.jsonl"] = {complete("c.jsonl", 0, 19, "J", "ccc", SourceKind::jsonl)};
    std::vector<ReaderInput> inputs{{SourceKind::txt, "a.txt", "raw"},
                                   {SourceKind::rss, "b.xml", "raw"},
                                   {SourceKind::jsonl, "c.jsonl", "raw"}};
    do {
        const auto report = report_of(read_documents(inputs, reader, {3, 4, 3, 12}, {}));
        check(report.counts.success == 3 && report.total_text_bytes == 12 && !report.stop,
              "all input permutations have identical count and inclusive text budget");
        check(report.items[0].record->doc_id.value() == 17 && report.items[1].record->doc_id.value() == 18 &&
              report.items[2].record->doc_id.value() == 19 && report.items[2].record->source.kind == SourceKind::jsonl,
              "TXT/RSS/JSONL scripted outcomes have deterministic sorted IDs and preserved kinds");
    } while (std::next_permutation(inputs.begin(), inputs.end(), [](const ReaderInput& left, const ReaderInput& right) {
        return left.input_path < right.input_path;
    }));
    reader.scripts["b.xml"] = {complete("b.xml", 0, 17, "R", "bbb", SourceKind::rss)};
    const auto duplicate = read_documents(inputs, reader, limits, continuing);
    check(!duplicate && duplicate.error().context == "document.reader: b.xml",
          "document IDs are unique across files, not only inside each input");
    reader.scripts["a.txt"] = {complete("a.txt", 0, 1, "A", "12345"), complete("a.txt", 1, 1, "B", "ok")};
    const auto reuse = report_of(read_documents({{SourceKind::txt, "a.txt", ""}}, reader, {3, 4, 2, 4}, continuing));
    check(reuse.counts.rejected == 1 && reuse.counts.success == 1 && reuse.total_text_bytes == 3,
          "rejected oversized candidate does not occupy delivered-ID set or text budget");
}

void adapter_contract_failures() {
    const auto rejected_case = [](std::vector<ReadItem> items, const char* label) {
        ScriptReader reader; reader.scripts["a.txt"] = std::move(items);
        const auto result = read_documents({{SourceKind::txt, "a.txt", ""}}, reader, limits, continuing);
        check(!result && result.error().context == "document.reader: a.txt", label);
    };
    auto item = complete("a.txt", 0, 1, "a", "b"); item.status = static_cast<ReadStatus>(99);
    rejected_case({item}, "invalid status is adapter contract failure");
    item = complete("a.txt", 0, 1, "a", "b"); item.record.reset();
    rejected_case({item}, "success must have payload");
    item = complete("a.txt", 0, 1, "a", "b"); item.issues = {{ReadIssueCode::missing_metadata, "missing", ""}};
    rejected_case({item}, "success must not hide warning diagnostics");
    item = complete("a.txt", 0, 1, "a", "b"); item.status = ReadStatus::warning;
    rejected_case({item}, "warning must include diagnostic");
    item = complete("a.txt", 0, 1, "a", "b"); item.status = ReadStatus::no_text; item.issues = {{ReadIssueCode::empty_content, "none", ""}};
    rejected_case({item}, "non-document status cannot deliver payload");
    item = diagnostic("a.txt", 0, ReadStatus::failed, ReadIssueCode::io_error, "");
    rejected_case({item}, "diagnostic reason must be non-empty");
    item = diagnostic("a.txt", 0, ReadStatus::failed, static_cast<ReadIssueCode>(99), "invalid");
    rejected_case({item}, "unknown issue code rejects contract");
    item = complete("a.txt", 0, 1, "a", "b"); item.status = ReadStatus::warning;
    item.issues = {{ReadIssueCode::partial_extraction, "some missing", ""}};
    rejected_case({item}, "partial warning must locate uncovered region");
    rejected_case({complete("a.txt", 0, 1, "a", "")}, "delivered content cannot be empty");
    rejected_case({complete("other.txt", 0, 1, "a", "b")}, "event source must match current input");
    item = complete("a.txt", 0, 1, "a", "b"); item.record->source.source_id.record_ordinal = 1;
    rejected_case({item}, "payload source must match event source");
    rejected_case({complete("a.txt", 0, 1, "a", "b", SourceKind::rss)}, "payload kind must match request");
    rejected_case({complete("a.txt", 1, 1, "a", "b"), complete("a.txt", 1, 2, "c", "d")}, "equal ordinal rejects");
    rejected_case({complete("a.txt", 2, 1, "a", "b"), complete("a.txt", 1, 2, "c", "d")}, "decreasing ordinal rejects");
    rejected_case({complete("a.txt", 0, 1, "a", "b"), complete("a.txt", 1, 1, "c", "d")}, "duplicate document identity rejects");
    rejected_case({}, "adapter cannot silently return no result");
    ActionReader ignores_stop([](const ReaderInput&, ReadSink& sink) {
        static_cast<void>(sink.emit(diagnostic("a.txt", 0, ReadStatus::failed, ReadIssueCode::io_error, "bad")));
        static_cast<void>(sink.emit(complete("a.txt", 1, 1, "a", "b")));
    });
    check(!read_documents({{SourceKind::txt, "a.txt", ""}}, ignores_stop, limits, {}),
          "ignoring false is a contract failure, not an ordinary partial report");
    ActionReader throws([](const ReaderInput&, ReadSink&) { throw std::runtime_error("fixture exception"); });
    bool observed = false;
    try { static_cast<void>(read_documents({{SourceKind::txt, "a.txt", ""}}, throws, limits, {})); }
    catch (const std::runtime_error& error) { observed = std::string{error.what()} == "fixture exception"; }
    check(observed, "unexpected adapter exceptions propagate rather than become false full success");
}

// 输入及 ScriptReader 全在局部销毁，返回报告仍须拥有正文/路径/诊断，供GDB按生命周期观察。
ReadReport escaped_report() {
    std::string input(4096, 'q');
    ScriptReader reader;
    auto item = complete("a.txt", 0, 7, "纸船", input);
    item.status = ReadStatus::warning;
    item.issues = {{ReadIssueCode::missing_metadata, "作者未提供", "author"}};
    reader.scripts["a.txt"] = {item};
    auto report = report_of(read_documents({{SourceKind::txt, "a.txt", input}}, reader, {4096, 8192, 2, 8192}, {}));
    input.clear(); reader.scripts.clear();
    return report;
}

void ownership_and_golden_input() {
    const auto escaped = escaped_report();
    check(escaped.items[0].record->content == std::string(4096, 'q') && escaped.items[0].record->title == "纸船",
          "report owns long body after borrowed input and adapter destruction");
    check(escaped.items[0].source_id.input_path == "a.txt" && escaped.items[0].issues[0].message == "作者未提供",
          "report owns source and diagnostic after producer destruction");
    auto copied = escaped; copied.items[0].record->content[0] = 'x';
    check(escaped.items[0].record->content[0] == 'q', "report copies own independent payload");
    auto moved = std::move(copied);
    check(moved.items[0].record->content[0] == 'x', "report move preserves destination payload");
    const std::string filename = std::string{SIFTWING_GOLDEN_DIR} + "/txt/en-river.txt";
    std::ifstream source(filename, std::ios::binary);
    if (!source) { throw std::runtime_error("golden input unavailable"); }
    const std::string bytes{std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
    const std::string expected = "A paper boat drifts beside the river gate.\nThe lantern is green.\n";
    check(bytes == expected && bytes.size() == 65, "read-only golden input matches independent literal bytes");
    ScriptReader reader; reader.scripts["txt/en-river.txt"] = {complete("txt/en-river.txt", 0, 17, "river", expected)};
    const auto report = report_of(read_documents({{SourceKind::txt, "txt/en-river.txt", bytes}}, reader, {65, 70, 1, 70}, {}));
    check(report.total_text_bytes == 70 && report.items[0].record->content == expected && !report.stop,
          "golden byte fixture verifies inclusive boundaries without production parsing");
    std::ifstream empty_file(std::string{SIFTWING_GOLDEN_DIR} + "/txt/empty.txt", std::ios::binary);
    if (!empty_file) { throw std::runtime_error("empty golden input unavailable"); }
    const std::string empty{std::istreambuf_iterator<char>{empty_file}, std::istreambuf_iterator<char>{}};
    check(empty.empty(), "zero-byte golden fixture is actually read");
    reader.scripts["txt/empty.txt"] = {diagnostic("txt/empty.txt", 0, ReadStatus::no_text, ReadIssueCode::empty_content, "empty fixture")};
    const auto no_text = report_of(read_documents({{SourceKind::txt, "txt/empty.txt", empty}}, reader, limits, {}));
    check(no_text.counts.no_text == 1 && !no_text.stop, "empty fixture outcome is explicit and not a fabricated document");
}

} // namespace

int main() {
    try {
        outcomes_and_order(); policy_and_partial(); input_and_document_limits();
        record_limits_and_failure_continuation(); request_validation(); multi_kind_and_cross_input_ids(); adapter_contract_failures();
        ownership_and_golden_input();
    } catch (const std::exception& error) {
        ++failures; std::cerr << "FAIL: unexpected test exception: " << error.what() << '\n';
    }
    std::cout << "document reader checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
