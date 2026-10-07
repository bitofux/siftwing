/*
 * PROJECT : SIFTWING
 * FILE    : scenarios.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 原创读取合同夹具，人工指定载荷、来源、诊断与预期状态
 */

#pragma once

#include "siftwing/document/document_reader.h"

#include <stdexcept>
#include <utility>

namespace siftwing::test_fixture {

inline document::ReadItem complete(std::string path, std::uint64_t ordinal, std::uint64_t id,
                                   std::string title, std::string content,
                                   document::SourceKind kind = document::SourceKind::txt) {
    auto identity = base::DocumentId::from_integer(id);
    if (!identity) { throw std::runtime_error("fixture identity must be representable"); }
    document::SourceIdentity source{std::move(path), ordinal};
    document::DocumentRecord record{std::move(identity).value(), std::move(title), std::move(content),
                                    {kind, source, {}, {}, {}}};
    return {source, document::ReadStatus::success, std::move(record), {}};
}

inline document::ReadItem diagnostic(std::string path, std::uint64_t ordinal,
                                     document::ReadStatus status, document::ReadIssueCode code,
                                     std::string reason) {
    return {{std::move(path), ordinal}, status, std::nullopt, {{code, std::move(reason), ""}}};
}

// 手工指定抽取未覆盖第2段；原始正文/范围不由被测收集器生成，验收必须保留这个警告。
inline document::ReadItem partial() {
    auto item = complete("articles/partial.txt", 0, 9, "纸船", "河边有一只纸船。\n");
    item.status = document::ReadStatus::warning;
    item.issues.push_back({document::ReadIssueCode::partial_extraction,
                          "第2段未能提取，当前正文只覆盖第1段", "paragraphs: 2"});
    return item;
}

} // namespace siftwing::test_fixture
