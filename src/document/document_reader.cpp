/*
 * PROJECT : SIFTWING
 * FILE    : document_reader.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 执行同步输入排序、读取结果合同检查、限额接纳和批量停止
 * IMPLEMENTATION :
 * -- 先验证全部请求，再调用适配器，配置错误没有局部业务副作用
 * -- 普通输入失败属于拥有型报告；适配器违约属于不可信批量 Result 失败
 * -- 所有计量在接纳前受检运算，超限不发布被截短的正文
 */

#include "siftwing/document/document_reader.h"

#include "siftwing/base/checked.h"

#include <algorithm>
#include <set>
#include <utility>

namespace siftwing::document {
namespace {

bool valid_kind(SourceKind kind) noexcept {
    switch (kind) {
    case SourceKind::txt:
    case SourceKind::rss:
    case SourceKind::jsonl: return true;
    }
    return false;
}

bool valid_path(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..") { return false; }
        if (end == std::string_view::npos) { return true; }
        begin = end + 1;
    }
    return false; // 尾部 / 不是逻辑文件标识。
}

bool valid_issue(ReadIssueCode code) noexcept {
    switch (code) {
    case ReadIssueCode::empty_content:
    case ReadIssueCode::unsupported_format:
    case ReadIssueCode::unsupported_encoding:
    case ReadIssueCode::input_too_large:
    case ReadIssueCode::document_too_large:
    case ReadIssueCode::encrypted:
    case ReadIssueCode::malformed_input:
    case ReadIssueCode::io_error:
    case ReadIssueCode::title_fallback:
    case ReadIssueCode::missing_metadata:
    case ReadIssueCode::unparsed_date:
    case ReadIssueCode::partial_extraction:
    case ReadIssueCode::partial_not_allowed: return true;
    }
    return false;
}

base::Error contract_error(std::string message, const std::string& path) {
    return {std::move(message), "document.reader: " + path};
}

class Collector final : public ReadSink {
public:
    Collector(const ReadLimits& limits, ReadPolicy policy) : limits_(limits), policy_(policy) {}

    void begin(const ReaderInput& input) {
        input_ = &input;
        previous_ordinal_.reset();
        calls_ = 0;
    }

    [[nodiscard]] bool accept(ReadItem item) override {
        if (error_) { return false; }
        if (report_.stop) { return fail("adapter emitted after stop"); }
        ++calls_;
        const bool has_document = item.status == ReadStatus::success || item.status == ReadStatus::warning;
        switch (item.status) {
        case ReadStatus::success:
        case ReadStatus::warning:
        case ReadStatus::no_text:
        case ReadStatus::rejected:
        case ReadStatus::failed: break;
        default: return fail("invalid read status");
        }
        if (item.source_id.input_path != input_->input_path ||
            (previous_ordinal_ && item.source_id.record_ordinal <= *previous_ordinal_)) {
            return fail("source mismatch or non-increasing record ordinal");
        }
        previous_ordinal_ = item.source_id.record_ordinal;
        if (has_document != item.record.has_value() ||
            ((item.status == ReadStatus::success) != item.issues.empty())) {
            return fail("status, payload and diagnostics disagree");
        }
        bool partial = false;
        for (const auto& issue : item.issues) {
            if (!valid_issue(issue.code) || issue.message.empty()) { return fail("invalid or empty diagnostic"); }
            if (issue.code == ReadIssueCode::partial_extraction) {
                if (issue.region.empty()) { return fail("partial extraction requires uncovered region"); }
                partial = true;
            }
        }
        std::uint64_t text_bytes = 0;
        if (item.record) {
            const auto& record = *item.record;
            if (record.source.kind != input_->kind ||
                record.source.source_id.input_path != item.source_id.input_path ||
                record.source.source_id.record_ordinal != item.source_id.record_ordinal || record.content.empty()) {
                return fail("document source mismatch or empty delivered content");
            }
            // 两个字符串长度分别窄化并先受检相加；不先计算 size_t 和再试图追回溢出。
            auto title_size = base::checked_narrow<std::uint64_t>(record.title.size(), "reader.title_bytes");
            auto content_size = base::checked_narrow<std::uint64_t>(record.content.size(), "reader.content_bytes");
            if (!title_size || !content_size) { return fail("text size is not representable"); }
            auto size = base::checked_add(title_size.value(), content_size.value(), "reader.document_bytes");
            if (!size) { return fail("document byte count overflow"); }
            text_bytes = size.value();
            if (partial && policy_.partial == PartialPolicy::reject) {
                item.status = ReadStatus::rejected;
                item.issues.push_back({ReadIssueCode::partial_not_allowed, "partial document rejected by policy", ""});
                item.record.reset();
                text_bytes = 0;
            } else if (text_bytes > limits_.max_document_bytes) {
                item.status = ReadStatus::rejected;
                item.issues.push_back({ReadIssueCode::document_too_large, "title plus content exceeds document byte limit", ""});
                item.record.reset();
                text_bytes = 0;
            }
        }
        if (report_.items.size() >= limits_.max_records) {
            stop(ReadStopReason::record_limit, item.source_id, "batch record limit reached; candidate not accepted");
            return false;
        }
        auto total = base::checked_add(report_.total_text_bytes, text_bytes, "reader.total_text_bytes");
        if (!total || total.value() > limits_.max_total_text_bytes) {
            stop(ReadStopReason::total_text_limit, item.source_id, "batch text limit reached; candidate not accepted");
            return false;
        }
        if (item.record && !ids_.insert(item.record->doc_id.value()).second) {
            return fail("duplicate delivered document ID");
        }
        // 已通过全部检查才提交统计和拥有载荷；普通拒绝/失败保留在报告，异常则没有返回报告。
        report_.total_text_bytes = total.value();
        switch (item.status) {
        case ReadStatus::success: ++report_.counts.success; break;
        case ReadStatus::warning: ++report_.counts.warning; break;
        case ReadStatus::no_text: ++report_.counts.no_text; break;
        case ReadStatus::rejected: ++report_.counts.rejected; break;
        case ReadStatus::failed: ++report_.counts.failed; break;
        }
        const bool failed = item.status == ReadStatus::rejected || item.status == ReadStatus::failed;
        report_.items.push_back(std::move(item));
        if (failed && policy_.failure == FailurePolicy::stop) {
            stop(ReadStopReason::failure_policy, report_.items.back().source_id, "stopped after rejected or failed record");
            return false;
        }
        return true;
    }

    bool failed_contract() const noexcept { return error_.has_value(); }
    bool stopped() const noexcept { return report_.stop.has_value(); }
    bool full() const noexcept { return report_.items.size() >= limits_.max_records; }
    bool emitted() const noexcept { return calls_ != 0; }
    void stop_before_input(const ReaderInput& input) {
        stop(ReadStopReason::record_limit, {input.input_path, 0}, "batch record limit reached before next input");
    }
    base::Result<ReadReport> finish() {
        if (error_) { return base::Result<ReadReport>::failure(std::move(*error_)); }
        return base::Result<ReadReport>::success(std::move(report_));
    }
    void no_outcome() { static_cast<void>(fail("adapter returned without a read outcome")); }

private:
    bool fail(std::string message) {
        error_ = contract_error(std::move(message), input_->input_path);
        return false;
    }
    void stop(ReadStopReason reason, SourceIdentity source, std::string message) {
        report_.stop = ReadStop{reason, std::move(source), std::move(message)};
    }

    const ReadLimits& limits_; // 同步调用内借用，不逃逸到返回报告。
    ReadPolicy policy_;
    const ReaderInput* input_ = nullptr; // begin 后仅当前 read 期间使用。
    std::optional<std::uint64_t> previous_ordinal_;
    std::uint64_t calls_ = 0;
    ReadReport report_;
    std::set<std::uint64_t> ids_; // 只包含真正接纳的文档，不为拒绝候选占用 ID。
    std::optional<base::Error> error_;
};

} // namespace

base::Result<ReadReport> read_documents(const std::vector<ReaderInput>& inputs, DocumentReader& reader,
                                      const ReadLimits& limits, ReadPolicy policy) {
    if (limits.max_input_bytes == 0 || limits.max_document_bytes == 0 ||
        limits.max_records == 0 || limits.max_total_text_bytes == 0) {
        return base::Result<ReadReport>::failure({"all read limits must be positive", "document.reader.limits"});
    }
    if ((policy.failure != FailurePolicy::stop && policy.failure != FailurePolicy::continue_reading) ||
        (policy.partial != PartialPolicy::reject && policy.partial != PartialPolicy::accept)) {
        return base::Result<ReadReport>::failure({"invalid read policy", "document.reader.policy"});
    }
    std::vector<const ReaderInput*> ordered;
    ordered.reserve(inputs.size());
    for (const auto& input : inputs) {
        if (!valid_kind(input.kind) || !valid_path(input.input_path)) {
            return base::Result<ReadReport>::failure(contract_error("invalid kind or relative input identifier", input.input_path));
        }
        ordered.push_back(&input);
    }
    std::sort(ordered.begin(), ordered.end(), [](const ReaderInput* left, const ReaderInput* right) {
        return left->input_path < right->input_path;
    });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        if (ordered[index - 1]->input_path == ordered[index]->input_path) {
            return base::Result<ReadReport>::failure(contract_error("duplicate input identifier", ordered[index]->input_path));
        }
    }
    Collector collector(limits, policy);
    for (const auto* input : ordered) {
        if (collector.full()) { collector.stop_before_input(*input); break; }
        collector.begin(*input);
        auto size = base::checked_narrow<std::uint64_t>(input->bytes.size(), "reader.input_bytes");
        if (!size || size.value() > limits.max_input_bytes) {
            static_cast<void>(collector.emit({{input->input_path, 0}, ReadStatus::rejected, std::nullopt,
                                              {{ReadIssueCode::input_too_large, "input exceeds byte limit", ""}}}));
        } else {
            reader.read(*input, limits, collector);
            if (!collector.emitted() && !collector.failed_contract()) { collector.no_outcome(); }
        }
        if (collector.failed_contract() || collector.stopped()) { break; }
    }
    return collector.finish();
}

} // namespace siftwing::document
