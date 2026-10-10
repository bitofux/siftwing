/*
 * PROJECT : SIFTWING
 * FILE    : rss_corpus_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 从受限stdin读取原创或私有RSS，以显式位置ID验收拥有型报告
 */

#include "siftwing/document/rss_reader.h"

#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {
using namespace siftwing::document;
std::uint64_t number(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) { throw std::invalid_argument("invalid unsigned integer"); }
    return value;
}
std::string hex(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const char value : text) {
        const auto byte = static_cast<unsigned char>(value);
        result.push_back(digits[byte >> 4U]); result.push_back(digits[byte & 15U]);
    }
    return result;
}
const char* status_name(ReadStatus status) {
    switch (status) {
    case ReadStatus::success: return "success"; case ReadStatus::warning: return "warning";
    case ReadStatus::no_text: return "no_text"; case ReadStatus::rejected: return "rejected";
    case ReadStatus::failed: return "failed";
    }
    throw std::logic_error("unknown status");
}
const char* issue_name(ReadIssueCode code) {
    switch (code) {
    case ReadIssueCode::empty_content: return "empty_content";
    case ReadIssueCode::unsupported_format: return "unsupported_format";
    case ReadIssueCode::unsupported_encoding: return "unsupported_encoding";
    case ReadIssueCode::input_too_large: return "input_too_large";
    case ReadIssueCode::document_too_large: return "document_too_large";
    case ReadIssueCode::encrypted: return "encrypted"; case ReadIssueCode::malformed_input: return "malformed_input";
    case ReadIssueCode::io_error: return "io_error"; case ReadIssueCode::title_fallback: return "title_fallback";
    case ReadIssueCode::missing_metadata: return "missing_metadata"; case ReadIssueCode::unparsed_date: return "unparsed_date";
    case ReadIssueCode::partial_extraction: return "partial_extraction"; case ReadIssueCode::partial_not_allowed: return "partial_not_allowed";
    }
    throw std::logic_error("unknown issue");
}
void optional_hex(const std::optional<std::string>& value) {
    if (value) { std::cout << '"' << hex(*value) << '"'; } else { std::cout << "null"; }
}
void print(const ReadReport& report) {
    std::cout << "{\"schema_version\":1,\"counts\":{\"success\":" << report.counts.success
              << ",\"warning\":" << report.counts.warning << ",\"no_text\":" << report.counts.no_text
              << ",\"rejected\":" << report.counts.rejected << ",\"failed\":" << report.counts.failed
              << "},\"total_text_bytes\":" << report.total_text_bytes << ",\"stop\":";
    if (report.stop) {
        const auto reason = report.stop->reason == ReadStopReason::record_limit ? "record_limit" :
                            report.stop->reason == ReadStopReason::total_text_limit ? "total_text_limit" : "failure_policy";
        std::cout << "{\"reason\":\"" << reason << "\",\"ordinal\":" << report.stop->source_id.record_ordinal << '}';
    } else { std::cout << "null"; }
    std::cout << ",\"items\":[";
    bool first = true;
    for (const auto& item : report.items) {
        if (!first) { std::cout << ','; } first = false;
        std::cout << "{\"ordinal\":" << item.source_id.record_ordinal << ",\"status\":\"" << status_name(item.status) << "\",\"record\":";
        if (item.record) {
            const auto& record = *item.record;
            std::cout << "{\"doc_id\":" << record.doc_id.value() << ",\"title_hex\":\"" << hex(record.title)
                      << "\",\"content_hex\":\"" << hex(record.content) << "\",\"url_hex\":";
            optional_hex(record.source.url); std::cout << ",\"author_hex\":"; optional_hex(record.source.author);
            std::cout << ",\"published_at_raw_hex\":"; optional_hex(record.source.published_at_raw); std::cout << '}';
        } else { std::cout << "null"; }
        std::cout << ",\"issues\":[";
        bool first_issue = true;
        for (const auto& diagnostic : item.issues) {
            if (!first_issue) { std::cout << ','; } first_issue = false;
            std::cout << "{\"code\":\"" << issue_name(diagnostic.code) << "\",\"region_hex\":\"" << hex(diagnostic.region) << "\"}";
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
} // namespace

int main(int argc, char** argv) {
    // 退出0仅表示可信报告，须检查逐条状态/stop；输入装载或合同错误1、配置错误2。
    if (argc != 13) { std::cerr << "usage: rss_probe count input document records total xml_nodes xml_depth html_input html_output html_tokens html_nodes html_depth\n"; return 2; }
    try {
        std::array<std::uint64_t, 12> values{};
        for (std::size_t i = 0; i < values.size(); ++i) { values[i] = number(argv[i + 1]); }
        if (values[0] > 1000000) { return 2; } // 验收工具ID配置的独立上限，不是生产身份分配合同。
        for (std::size_t i = 1; i < values.size(); ++i) { if (values[i] == 0) { return 2; } }
        const ReadLimits limits{values[1], values[2], values[3], values[4]};
        const RssParseLimits parsing{values[5], values[6], {values[7], values[8], values[9], values[10], values[11]}};
        std::vector<siftwing::base::DocumentId> ids;
        for (std::uint64_t i = 0; i < values[0]; ++i) { ids.push_back(siftwing::base::DocumentId::from_integer(i, "probe.id").value()); }
        RssDocumentReader reader({{"feed.xml", std::move(ids)}}, parsing);
        std::string bytes;
        std::array<char, 4096> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > limits.max_input_bytes - bytes.size()) { std::cout << "{\"error_context\":\"probe.input_bytes\"}\n"; return 1; }
            bytes.append(buffer.data(), count);
        }
        if (!std::cin.eof()) { return 1; }
        auto result = read_documents({{SourceKind::rss, "feed.xml", bytes}}, reader, limits,
                                     {FailurePolicy::continue_reading, PartialPolicy::reject});
        if (!result) { std::cout << "{\"error_context_hex\":\"" << hex(result.error().context) << "\"}\n"; return 1; }
        print(result.value());
        return std::cout ? 0 : 1;
    } catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
