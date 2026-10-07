/*
 * PROJECT : SIFTWING
 * FILE    : document_record_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 验证统一文档的拥有关系、值语义、来源定位和缺失/空值边界
 */

#include "siftwing/document/document_record.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using siftwing::base::DocumentId;
using siftwing::document::DocumentRecord;
using siftwing::document::DocumentSource;
using siftwing::document::SourceIdentity;
using siftwing::document::SourceKind;

static_assert(std::is_aggregate_v<SourceIdentity>);
static_assert(std::is_aggregate_v<DocumentSource>);
static_assert(std::is_aggregate_v<DocumentRecord>);
static_assert(!std::is_default_constructible_v<DocumentRecord>);
static_assert(std::is_copy_constructible_v<DocumentRecord>);
static_assert(std::is_copy_assignable_v<DocumentRecord>);
static_assert(std::is_nothrow_move_constructible_v<DocumentRecord>);
static_assert(std::is_nothrow_move_assignable_v<DocumentRecord>);
static_assert(std::is_nothrow_destructible_v<DocumentRecord>);
static_assert(std::is_same_v<decltype(DocumentRecord::doc_id), DocumentId>);
static_assert(std::is_same_v<decltype(DocumentRecord::title), std::string>);
static_assert(std::is_same_v<decltype(DocumentRecord::content), std::string>);
static_assert(std::is_same_v<decltype(DocumentSource::url), std::optional<std::string>>);
static_assert(std::is_same_v<decltype(SourceIdentity::record_ordinal), std::uint64_t>);

std::size_t checks = 0;
std::size_t failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

DocumentId identity(std::uint64_t value) {
    auto result = DocumentId::from_integer(value);
    if (!result) {
        throw std::runtime_error("test identity must be representable");
    }
    return std::move(result).value();
}

// 局部字符串在返回前被改写、返回后销毁；长正文排除仅依靠短字符串偶然保留字节的假象。
DocumentRecord escaped_record() {
    std::string title = "河边纸船";
    std::string content(4096, 'x');
    content.replace(100, 4, std::string{"a\0b\n", 4});
    std::string path = "feeds/workshop.xml";
    std::string url = "https://fixtures.example.invalid/boat";
    std::string author = "原创作者";
    std::string date = "Mon, 05 Oct 2026 10:00:00 GMT";
    DocumentRecord record{identity(7), title, content,
                          {SourceKind::rss, {path, 1}, url, author, date}};
    title.assign("changed");
    content.clear();
    path.clear();
    url.clear();
    author.clear();
    date.clear();
    return record;
}

void ownership_and_metadata() {
    const auto record = escaped_record();
    check(record.doc_id.value() == 7, "caller-provided identity survives source lifetime");
    check(record.title == "河边纸船", "title owns UTF-8 bytes after local input destruction");
    check(record.content.size() == 4096 && record.content.substr(100, 4) == std::string{"a\0b\n", 4},
          "content owns the complete byte sequence including NUL and newline");
    check(record.content.front() == 'x' && record.content.back() == 'x', "long content boundaries preserved");
    check(record.source.kind == SourceKind::rss, "source kind stored independently of content");
    check(record.source.source_id.input_path == "feeds/workshop.xml", "source path owns its bytes");
    check(record.source.source_id.record_ordinal == 1, "source ordinal is not the document identity");
    check(record.source.url && *record.source.url == "https://fixtures.example.invalid/boat", "URL is owned metadata");
    check(record.source.author && *record.source.author == "原创作者", "author is owned metadata");
    check(record.source.published_at_raw && *record.source.published_at_raw == "Mon, 05 Oct 2026 10:00:00 GMT",
          "date retains its original text rather than a parsed timestamp");
    check(record.content.find("fixtures.example.invalid") == std::string::npos &&
              record.content.find("原创作者") == std::string::npos,
          "source metadata is not automatically appended to content");
}

void empty_and_missing_values() {
    const DocumentRecord empty{identity(0), "", "", {}};
    check(empty.doc_id.value() == 0, "zero remains a legal explicit document identity");
    check(empty.title.empty() && empty.content.empty(), "container can represent empty title and content");
    check(empty.source.kind == SourceKind::txt && empty.source.source_id.record_ordinal == 0,
          "empty source assembly has documented TXT and zero defaults");
    check(empty.source.source_id.input_path.empty(), "source defaults are storage, not successful validation");
    check(!empty.source.url && !empty.source.author && !empty.source.published_at_raw,
          "default optional metadata is absent");

    DocumentRecord supplied_empty{identity(std::numeric_limits<std::uint64_t>::max()), "", "",
                                 {SourceKind::jsonl, {"records/input.jsonl", 0},
                                  std::string{}, std::string{}, std::string{}}};
    check(supplied_empty.doc_id.value() == std::numeric_limits<std::uint64_t>::max(), "maximum identity accepted unchanged");
    check(supplied_empty.source.url && supplied_empty.source.url->empty(), "present empty URL differs from absence");
    check(supplied_empty.source.author && supplied_empty.source.author->empty(), "present empty author differs from absence");
    check(supplied_empty.source.published_at_raw && supplied_empty.source.published_at_raw->empty(),
          "present empty raw date differs from absence");
    supplied_empty.source.published_at_raw = "unparsed date";
    check(*supplied_empty.source.published_at_raw == "unparsed date", "container does not pretend to parse dates");
    supplied_empty.source.url.reset();
    check(!supplied_empty.source.url, "caller can explicitly replace supplied metadata with absence");
}

void source_and_identity_boundaries() {
    const DocumentRecord first{identity(42), "same title", "first", {SourceKind::txt, {"texts/a.txt", 0}, {}, {}, {}}};
    const DocumentRecord second{identity(99), "same title", "second", {SourceKind::txt, {"texts/a.txt", 1}, {}, {}, {}}};
    const DocumentRecord other_file{identity(42), "same title", "third", {SourceKind::txt, {"texts/b.txt", 0}, {}, {}, {}}};
    check(first.source.source_id.input_path == second.source.source_id.input_path &&
              first.source.source_id.record_ordinal != second.source.source_id.record_ordinal,
          "same file records remain individually traceable");
    check(first.source.source_id.input_path != other_file.source.source_id.input_path &&
              first.source.source_id.record_ordinal == other_file.source.source_id.record_ordinal,
          "source identity includes the path as well as the ordinal");
    check(first.title == second.title && first.doc_id != second.doc_id, "title does not determine internal identity");
    check(first.doc_id == other_file.doc_id, "container does not silently allocate or enforce global uniqueness");
    check(first.doc_id.value() != first.source.source_id.record_ordinal, "internal ID is not the file ordinal");
    const SourceIdentity high_ordinal{"feeds/many.xml", std::numeric_limits<std::uint64_t>::max()};
    check(high_ordinal.record_ordinal == std::numeric_limits<std::uint64_t>::max(), "ordinal stores its complete value domain");
}

void value_semantics() {
    const auto original = escaped_record();
    auto copy = original;
    copy.doc_id = identity(8);
    copy.title.assign("copy title");
    copy.content.assign("copy content");
    copy.source.kind = SourceKind::jsonl;
    copy.source.source_id.input_path.assign("records/copy.jsonl");
    copy.source.source_id.record_ordinal = 3;
    copy.source.url = "copy URL";
    copy.source.author.reset();
    copy.source.published_at_raw = "copy date";
    check(original.doc_id.value() == 7 && copy.doc_id.value() == 8, "copied identities can be assigned independently");
    check(original.title == "河边纸船" && copy.title == "copy title", "copy owns an independent title");
    check(original.content.size() == 4096 && copy.content == "copy content", "copy owns independent content");
    check(original.source.kind == SourceKind::rss && copy.source.kind == SourceKind::jsonl, "source kind copied by value");
    check(original.source.source_id.input_path == "feeds/workshop.xml" && copy.source.source_id.input_path == "records/copy.jsonl",
          "source paths copied independently");
    check(original.source.source_id.record_ordinal == 1 && copy.source.source_id.record_ordinal == 3,
          "source ordinals copied independently");
    check(original.source.url && *original.source.url == "https://fixtures.example.invalid/boat" && *copy.source.url == "copy URL",
          "optional URL owns independent text");
    check(original.source.author && !copy.source.author, "optional presence does not alias the original");
    check(*original.source.published_at_raw == "Mon, 05 Oct 2026 10:00:00 GMT" && *copy.source.published_at_raw == "copy date",
          "raw dates copied independently");

    DocumentRecord assigned{identity(0), "", "", {}};
    assigned = original;
    assigned.content[0] = 'z';
    check(assigned.doc_id == original.doc_id && original.content[0] == 'x' && assigned.content[0] == 'z',
          "copy assignment replaces fields without aliasing source storage");
    auto moved = std::move(copy);
    check(moved.doc_id.value() == 8 && moved.title == "copy title" && moved.content == "copy content", "move construction preserves target payload");
    check(moved.source.source_id.input_path == "records/copy.jsonl" && moved.source.source_id.record_ordinal == 3,
          "move construction preserves target source identity");
    check(*moved.source.url == "copy URL" && !moved.source.author && *moved.source.published_at_raw == "copy date",
          "move construction preserves optional metadata on target");
    // 只验证源仍可赋值和析构，不依赖标准未保证的 moved-from 字符串内容。
    copy = original;
    check(copy.content == original.content && copy.source.author == original.source.author, "moved-from record remains assignable");
    assigned = std::move(moved);
    check(assigned.doc_id.value() == 8 && assigned.content == "copy content" && *assigned.source.url == "copy URL",
          "move assignment replaces the complete target payload");
    moved = original;
    check(moved.title == original.title, "move-assigned source remains assignable");

    std::vector<DocumentRecord> records;
    records.push_back(original);
    records.push_back(std::move(assigned));
    check(records.size() == 2 && records[0].doc_id.value() == 7 && records[1].doc_id.value() == 8,
          "records support ordinary owned container storage");
    records[0].source.source_id.input_path.assign("other.xml");
    check(original.source.source_id.input_path == "feeds/workshop.xml", "container copy does not alias source metadata");
}

}  // namespace

int main() {
    ownership_and_metadata();
    empty_and_missing_values();
    source_and_identity_boundaries();
    value_semantics();
    std::cout << checks << " document record checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
