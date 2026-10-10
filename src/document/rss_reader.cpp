/*
 * PROJECT : SIFTWING
 * FILE    : rss_reader.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用tinyxml2读取RSS字段，复用HTML抽取并移交有界拥有型文档
 * IMPLEMENTATION :
 * -- XML字符/实体/DTD支持范围先预检，完整语法和树构造交给tinyxml2
 * -- 命名空间按祖先声明解析，字段连接全部Text/CDATA，不用GetText遗漏后续片段
 * -- 全文结构/身份数量先核对，再逐item抽取；sink拒绝后立即停止
 */

#include "siftwing/document/rss_reader.h"
#include "siftwing/base/checked.h"

#include <tinyxml2.h>
#include <utf8.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

namespace siftwing::document {
namespace {

using Element = tinyxml2::XMLElement;
using Node = tinyxml2::XMLNode;
constexpr std::string_view content_uri = "http://purl.org/rss/1.0/modules/content/";
constexpr std::string_view dc_uri = "http://purl.org/dc/elements/1.1/";
constexpr std::string_view xml_uri = "http://www.w3.org/XML/1998/namespace";
constexpr std::string_view xmlns_uri = "http://www.w3.org/2000/xmlns/";

struct Problem { ReadStatus status; ReadIssueCode code; std::string message; std::string region; };

Problem malformed(std::string message, std::string region) {
    return {ReadStatus::failed, ReadIssueCode::malformed_input, std::move(message), std::move(region)};
}
Problem unsupported(std::string message, std::string region) {
    return {ReadStatus::rejected, ReadIssueCode::unsupported_format, std::move(message), std::move(region)};
}

bool space(char value) noexcept { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; }
bool xml_character(std::uint32_t value) noexcept {
    return value == 9 || value == 10 || value == 13 || (value >= 0x20U && value <= 0xd7ffU) ||
           (value >= 0xe000U && value <= 0xfffdU) || (value >= 0x10000U && value <= 0x10ffffU);
}
bool white_space(std::uint32_t point) noexcept {
    return (point >= 9 && point <= 13) || point == 0x20U || point == 0x85U || point == 0xa0U ||
           point == 0x1680U || (point >= 0x2000U && point <= 0x200aU) || point == 0x2028U ||
           point == 0x2029U || point == 0x202fU || point == 0x205fU || point == 0x3000U;
}
std::string_view trim(std::string_view text) {
    auto current = text.begin();
    auto first = text.end();
    auto last = text.end();
    while (current != text.end()) {
        const auto start = current;
        if (!white_space(utf8::next(current, text.end()))) {
            if (first == text.end()) { first = start; }
            last = current;
        }
    }
    return first == text.end() ? std::string_view{} : std::string_view{first, static_cast<std::size_t>(last - first)};
}
bool starts(std::string_view text, std::size_t position, std::string_view prefix) noexcept {
    return text.substr(position, prefix.size()) == prefix;
}
std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (auto& c : result) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    return result;
}

// 只检查本模块接纳的XML声明，不替代元素/属性语法解析。
std::optional<Problem> declaration(std::string_view text) {
    std::size_t position = 3;
    unsigned stage = 0;
    std::set<std::string_view> names;
    while (position < text.size()) {
        if (!space(text[position])) { return malformed("invalid XML declaration separator", "xml.declaration"); }
        while (position < text.size() && space(text[position])) { ++position; }
        if (position == text.size()) { break; }
        const auto begin = position;
        while (position < text.size() && !space(text[position]) && text[position] != '=') { ++position; }
        const auto name = text.substr(begin, position - begin);
        while (position < text.size() && space(text[position])) { ++position; }
        if (position == text.size() || text[position++] != '=') { return malformed("invalid XML declaration attribute", "xml.declaration"); }
        while (position < text.size() && space(text[position])) { ++position; }
        if (position == text.size() || (text[position] != '\'' && text[position] != '"')) { return malformed("XML declaration value must be quoted", "xml.declaration"); }
        const char quote = text[position++];
        const auto end = text.find(quote, position);
        if (end == text.npos || !names.insert(name).second) { return malformed("duplicate or incomplete XML declaration", "xml.declaration"); }
        const auto value = text.substr(position, end - position);
        position = end + 1;
        if (stage == 0 && name != "version") { return malformed("XML version must be declared first", "xml.declaration"); }
        if (name == "version" && stage == 0) {
            if (value != "1.0") { return unsupported("only XML 1.0 is supported", "xml.version"); }
            stage = 1;
        } else if (name == "encoding" && stage == 1) {
            if (lower_ascii(value) != "utf-8") { return Problem{ReadStatus::rejected, ReadIssueCode::unsupported_encoding, "only declared UTF-8 is supported", "xml.encoding"}; }
            stage = 2;
        } else if (name == "standalone" && stage <= 2 && (value == "yes" || value == "no")) {
            stage = 3;
        } else { return malformed("unsupported XML declaration attributes or order", "xml.declaration"); }
    }
    if (stage == 0) { return malformed("XML declaration lacks version", "xml.declaration"); }
    return {};
}

bool valid_entity(std::string_view name) noexcept {
    if (name == "amp" || name == "lt" || name == "gt" || name == "apos" || name == "quot") { return true; }
    if (name.empty() || name.front() != '#') { return false; }
    name.remove_prefix(1);
    std::uint32_t base = 10;
    if (!name.empty() && name.front() == 'x') { base = 16; name.remove_prefix(1); }
    if (name.empty()) { return false; }
    std::uint32_t value = 0;
    for (const char c : name) {
        std::uint32_t digit = 16;
        if (c >= '0' && c <= '9') { digit = static_cast<std::uint32_t>(c - '0'); }
        else if (c >= 'a' && c <= 'f') { digit = static_cast<std::uint32_t>(c - 'a' + 10); }
        else if (c >= 'A' && c <= 'F') { digit = static_cast<std::uint32_t>(c - 'A' + 10); }
        if (digit >= base || value > (0x10ffffU - digit) / base) { return false; }
        value = value * base + digit;
    }
    return xml_character(value);
}

// XML1.0第五版Name范围；命名空间的每一部分另禁止冒号，不能只依赖库的字节分类。
bool name_start(std::uint32_t c) noexcept {
    return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= 0xc0U && c <= 0xd6U) || (c >= 0xd8U && c <= 0xf6U) ||
        (c >= 0xf8U && c <= 0x2ffU) || (c >= 0x370U && c <= 0x37dU) ||
        (c >= 0x37fU && c <= 0x1fffU) || (c >= 0x200cU && c <= 0x200dU) ||
        (c >= 0x2070U && c <= 0x218fU) || (c >= 0x2c00U && c <= 0x2fefU) ||
        (c >= 0x3001U && c <= 0xd7ffU) || (c >= 0xf900U && c <= 0xfdcfU) ||
        (c >= 0xfdf0U && c <= 0xfffdU) || (c >= 0x10000U && c <= 0xeffffU);
}
bool valid_name(std::string_view name, bool allow_colon) {
    if (name.empty()) { return false; }
    auto it = name.begin();
    bool first = true;
    while (it != name.end()) {
        const auto c = utf8::next(it, name.end());
        if (c == ':' && !allow_colon) { return false; }
        if (!name_start(c) && (first || !(c == '-' || c == '.' || (c >= '0' && c <= '9') ||
              c == 0xb7U || (c >= 0x300U && c <= 0x36fU) || (c >= 0x203fU && c <= 0x2040U)))) { return false; }
        first = false;
    }
    return true;
}

std::optional<Problem> preflight(std::string_view bytes, std::string& parser_bytes) {
    auto scalar = bytes.begin();
    while (scalar != bytes.end()) {
        if (!xml_character(utf8::next(scalar, bytes.end()))) { return malformed("input contains a forbidden XML 1.0 character", "xml.characters"); }
    }
    bool in_tag = false;
    bool after_attribute = false;
    std::size_t copied = 0;
    char quote = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '&') {
            const auto end = bytes.find(';', i + 1);
            if (end == bytes.npos || !valid_entity(bytes.substr(i + 1, end - i - 1))) {
                return malformed("invalid or undeclared XML entity", "xml.entity byte: " + std::to_string(i));
            }
            i = end;
            continue;
        }
        if (quote != 0) {
            if (bytes[i] == '<') { return malformed("literal less-than in XML attribute", "xml.attribute"); }
            if (bytes[i] == quote) { quote = 0; after_attribute = true; }
            continue;
        }
        if (in_tag) {
            if (after_attribute && !space(bytes[i]) && bytes[i] != '/' && bytes[i] != '>') {
                return malformed("XML attributes require whitespace separators", "xml.attribute");
            }
            after_attribute = false;
            if (bytes[i] == '\'' || bytes[i] == '"') { quote = bytes[i]; }
            else if (bytes[i] == '>') { in_tag = false; }
            else if (bytes[i] == '<') { return malformed("nested opening delimiter in XML tag", "xml.tag"); }
            continue;
        }
        if (starts(bytes, i, "<!--")) {
            const auto end = bytes.find("-->", i + 4);
            if (end == bytes.npos || bytes.substr(i + 4, end - i - 4).find("--") != bytes.npos || (end > i + 4 && bytes[end - 1] == '-')) { return malformed("invalid XML comment", "xml.comment"); }
            i = end + 2;
        } else if (starts(bytes, i, "<![CDATA[")) {
            const auto end = bytes.find("]]>", i + 9);
            if (end == bytes.npos) { return malformed("unterminated CDATA", "xml.cdata"); }
            i = end + 2;
        } else if (starts(bytes, i, "<!")) { return unsupported("DTD and markup declarations are disabled", "xml.dtd"); }
        else if (starts(bytes, i, "<?")) {
            const auto end = bytes.find("?>", i + 2);
            if (end == bytes.npos) { return malformed("unterminated XML processing instruction", "xml.pi"); }
            const auto body = bytes.substr(i + 2, end - i - 2);
            const auto target_end = body.find_first_of(" \t\r\n");
            const auto target = body.substr(0, target_end);
            if (!valid_name(target, true)) { return malformed("invalid XML processing instruction target", "xml.pi"); }
            if (lower_ascii(target) == "xml") {
                if (target != "xml" || i != 0) { return malformed("XML declaration must occur at the start", "xml.declaration"); }
                if (auto error = declaration(body)) { return error; }
            } else {
                // tinyxml2将所有<?...?>视作声明；已验证的PI换成一个忽略注释节点。
                // 保留换行数以维持后续错误行号，仍计一个DOM节点，不执行或消费PI数据。
                parser_bytes.append(bytes.substr(copied, i - copied));
                parser_bytes += "<!--";
                for (const char c : body) { if (c == '\n' || c == '\r') { parser_bytes += c; } }
                parser_bytes += "-->";
                copied = end + 2;
            }
            i = end + 1;
        } else if (starts(bytes, i, "]]>")) { return malformed("CDATA closing delimiter in ordinary XML text", "xml.text"); }
        else if (bytes[i] == '<') { in_tag = true; }
    }
    if (copied != 0) { parser_bytes.append(bytes.substr(copied)); }
    return {};
}

struct QName { std::string_view prefix; std::string_view local; };
std::optional<QName> qualified(std::string_view name) {
    const auto colon = name.find(':');
    if (colon == name.npos) { return valid_name(name, false) ? std::optional<QName>{QName{{}, name}} : std::nullopt; }
    if (!valid_name(name.substr(0, colon), false) || !valid_name(name.substr(colon + 1), false)) { return {}; }
    return QName{name.substr(0, colon), name.substr(colon + 1)};
}
std::optional<std::string_view> namespace_uri(const Element* element, std::string_view prefix) {
    if (prefix == "xml") { return xml_uri; }
    const std::string attribute = prefix.empty() ? "xmlns" : "xmlns:" + std::string(prefix);
    for (auto* scope = element; scope != nullptr; scope = scope->Parent() == nullptr ? nullptr : scope->Parent()->ToElement()) {
        if (const auto* uri = scope->Attribute(attribute.c_str())) { return uri; }
    }
    return prefix.empty() ? std::optional<std::string_view>{""} : std::nullopt;
}
bool namespaces_valid(const Element* element) {
    const auto name = qualified(element->Name());
    if (!name || name->prefix == "xmlns" || !namespace_uri(element, name->prefix)) { return false; }
    std::set<std::pair<std::string, std::string>> expanded_attributes;
    for (auto* attr = element->FirstAttribute(); attr != nullptr; attr = attr->Next()) {
        const auto part = qualified(attr->Name());
        if (!part) { return false; }
        if (part->prefix == "xmlns" || (part->prefix.empty() && part->local == "xmlns")) {
            const std::string_view uri{attr->Value()};
            const auto prefix = part->prefix.empty() ? std::string_view{} : part->local;
            if (prefix == "xmlns" || uri == xmlns_uri || (prefix == "xml" && uri != xml_uri) ||
                (prefix != "xml" && uri == xml_uri) || (!prefix.empty() && uri.empty())) { return false; }
            continue;
        }
        const auto uri = part->prefix.empty() ? std::optional<std::string_view>{""} : namespace_uri(element, part->prefix);
        if (!uri || !expanded_attributes.emplace(std::string(*uri), std::string(part->local)).second) { return false; }
    }
    return true;
}
bool matches(const Element* element, std::string_view uri, std::string_view local) {
    const auto name = qualified(element->Name());
    return name && name->local == local && namespace_uri(element, name->prefix) == uri;
}

// 祖先路径遍历，宽树不一次性压入全部兄弟；所有节点在发布第一个item前检查。
std::optional<Problem> validate_tree(const tinyxml2::XMLDocument& document, const RssParseLimits& limits) {
    struct Frame { const Node* next; std::uint64_t depth; };
    std::vector<Frame> path{{document.FirstChild(), 0}};
    std::uint64_t count = 0;
    while (!path.empty()) {
        auto& frame = path.back();
        if (!frame.next) { path.pop_back(); continue; }
        if (frame.depth >= limits.max_xml_depth) { return unsupported("XML depth is outside configured parsing support", "xml.depth"); }
        if (count >= limits.max_xml_nodes) { return unsupported("XML nodes are outside configured parsing support", "xml.nodes"); }
        ++count;
        const auto* node = frame.next;
        const auto depth = frame.depth + 1;
        frame.next = node->NextSibling();
        if (node->ToUnknown()) { return unsupported("XML markup declarations are disabled", "xml.dtd"); }
        if (const auto* element = node->ToElement(); element && !namespaces_valid(element)) { return malformed("invalid XML namespace binding or qualified name", "xml.namespace"); }
        if (frame.depth == 0 && node->ToText()) {
            const std::string_view text{node->Value()};
            if (node->ToText()->CData() || !std::all_of(text.begin(), text.end(), space)) { return malformed("text outside XML root", "xml.root"); }
        }
        path.push_back({node->FirstChild(), depth});
    }
    return {};
}

struct Field { const Element* node = nullptr; bool duplicate = false; };
Field find_field(const Element* parent, std::string_view uri, std::string_view local) {
    Field result;
    for (auto* child = parent->FirstChildElement(); child != nullptr; child = child->NextSiblingElement()) {
        if (matches(child, uri, local)) { if (result.node) { result.duplicate = true; } else { result.node = child; } }
    }
    return result;
}
struct Text { std::string value; std::optional<Problem> error; };
Text field_text(const Element* element, std::uint64_t limit, const std::string& region) {
    Text result;
    if (!element) { return result; }
    for (auto* node = element->FirstChild(); node != nullptr; node = node->NextSibling()) {
        if (const auto* text = node->ToText()) {
            const std::string_view part{text->Value()};
            if (part.size() > limit - result.value.size()) {
                result.error = Problem{ReadStatus::rejected, ReadIssueCode::input_too_large, "decoded RSS field exceeds byte limit", region};
                return result;
            }
            result.value.append(part);
        } else if (node->ToElement() || node->ToUnknown()) { result.error = unsupported("consumed RSS field must contain only text/CDATA", region); return result; }
    }
    return result;
}

ReadItem problem_item(const SourceIdentity& source, Problem error) {
    return {source, error.status, {}, {{error.code, std::move(error.message), std::move(error.region)}}};
}

ReadItem extract_item(const Element* element, const ReaderInput& input, std::uint64_t ordinal,
                      base::DocumentId id, const ReadLimits& limits, const RssParseLimits& parsing) {
    const SourceIdentity source{input.input_path, ordinal};
    const std::string region = "item[" + std::to_string(ordinal) + "]/";
    const std::array<Field, 8> fields{{find_field(element, content_uri, "encoded"), find_field(element, "", "content"),
        find_field(element, "", "description"), find_field(element, "", "title"), find_field(element, "", "link"),
        find_field(element, "", "pubDate"), find_field(element, dc_uri, "creator"), find_field(element, "", "author")}};
    const std::array<const char*, 8> names{{"content:encoded", "content", "description", "title", "link", "pubDate", "dc:creator", "author"}};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].duplicate) { return problem_item(source, unsupported("duplicate recognized RSS field", region + names[index])); }
    }
    std::string content;
    std::vector<ReadIssue> issues;
    for (std::size_t index = 0; index < 3; ++index) {
        if (!fields[index].node) { continue; }
        auto raw = field_text(fields[index].node, parsing.html.max_input_bytes, region + names[index]);
        if (raw.error) { return problem_item(source, std::move(*raw.error)); }
        auto html_limits = parsing.html;
        html_limits.max_output_bytes = std::min(html_limits.max_output_bytes, limits.max_document_bytes);
        auto extracted = extract_html_text(raw.value, html_limits);
        if (!extracted) {
            const auto& context = extracted.error().context;
            const auto code = context == "html.output_bytes" ? ReadIssueCode::document_too_large :
                              context == "html.input_bytes" ? ReadIssueCode::input_too_large : ReadIssueCode::unsupported_format;
            return problem_item(source, {ReadStatus::rejected, code, extracted.error().message, region + names[index] + ":" + context});
        }
        if (extracted.value().text.empty()) { continue; }
        if (index != 0) { issues.push_back({ReadIssueCode::missing_metadata, "preferred body absent or empty; selected fallback field", region + names[index]}); }
        if (extracted.value().recovered) { issues.push_back({ReadIssueCode::malformed_input, "HTML parser recovered syntax; completeness is not guaranteed", region + names[index]}); }
        content = std::move(extracted).value().text;
        break;
    }
    if (content.empty()) { return problem_item(source, {ReadStatus::no_text, ReadIssueCode::empty_content, "RSS item has no extractable body", region + "body"}); }
    auto title_text = field_text(fields[3].node, limits.max_input_bytes, region + "title");
    if (title_text.error) { return problem_item(source, std::move(*title_text.error)); }
    auto title = trim(title_text.value);
    std::string fallback;
    if (title.empty()) {
        const auto slash = input.input_path.rfind('/');
        std::string_view stem{input.input_path};
        if (slash != stem.npos) { stem.remove_prefix(slash + 1); }
        const auto dot = stem.rfind('.');
        if (dot != stem.npos && dot != 0) { stem = stem.substr(0, dot); }
        if (utf8::find_invalid(stem.begin(), stem.end()) != stem.end() || stem.find('\0') != stem.npos || trim(stem).empty()) {
            return problem_item(source, {ReadStatus::rejected, ReadIssueCode::malformed_input, "invalid RSS fallback filename", region + "title"});
        }
        const std::string suffix = "#" + std::to_string(ordinal);
        if (stem.size() > limits.max_document_bytes || suffix.size() > limits.max_document_bytes - stem.size() ||
            content.size() > limits.max_document_bytes - stem.size() - suffix.size()) {
            return problem_item(source, {ReadStatus::rejected, ReadIssueCode::document_too_large, "RSS title plus body exceeds document limit", region + "title"});
        }
        fallback = std::string(stem) + suffix;
        title = fallback;
        issues.push_back({ReadIssueCode::title_fallback, "RSS title uses filename and original item ordinal", region + "title"});
    }
    if (title.size() > limits.max_document_bytes || content.size() > limits.max_document_bytes - title.size()) {
        return problem_item(source, {ReadStatus::rejected, ReadIssueCode::document_too_large, "RSS title plus body exceeds document limit", region + "title+body"});
    }
    // 先确定最终标题+正文预算，再复制输出标题/元数据；临时XML字段缓冲受输入预算限制。
    std::array<std::optional<std::string>, 3> metadata;
    const std::array<std::size_t, 3> indices{{4, fields[6].node ? 6U : 7U, 5}};
    for (std::size_t index = 0; index < indices.size(); ++index) {
        const auto field = indices[index];
        if (fields[field].node) {
            auto text = field_text(fields[field].node, limits.max_input_bytes, region + names[field]);
            if (text.error) { return problem_item(source, std::move(*text.error)); }
            metadata[index] = std::string(trim(text.value));
        }
    }
    DocumentRecord record{id, std::string(title), std::move(content),
        {SourceKind::rss, source, std::move(metadata[0]), std::move(metadata[1]), std::move(metadata[2])}};
    return {source, issues.empty() ? ReadStatus::success : ReadStatus::warning, std::move(record), std::move(issues)};
}

} // namespace

RssDocumentReader::RssDocumentReader(std::map<std::string, std::vector<base::DocumentId>> identities,
                                     RssParseLimits limits)
    : identities_(std::move(identities)), limits_(limits) {
    const auto& html = limits_.html;
    if (limits_.max_xml_nodes == 0 || limits_.max_xml_depth == 0 || html.max_input_bytes == 0 ||
        html.max_output_bytes == 0 || html.max_tokens == 0 || html.max_nodes == 0 || html.max_depth == 0) {
        throw std::invalid_argument("RSS parsing requires positive XML and HTML budgets");
    }
}

void RssDocumentReader::read(const ReaderInput& input, const ReadLimits& limits, ReadSink& sink) {
    if (limits.max_input_bytes == 0 || limits.max_document_bytes == 0 || limits.max_records == 0 || limits.max_total_text_bytes == 0) {
        throw std::invalid_argument("RSS read requires positive acceptance limits");
    }
    const SourceIdentity file_source{input.input_path, 0};
    const auto reject = [&](Problem error) { static_cast<void>(sink.emit(problem_item(file_source, std::move(error)))); };
    if (input.kind != SourceKind::rss) { reject(unsupported("RSS reader accepts only RSS inputs", "rss.kind")); return; }
    const auto size = base::checked_narrow<std::uint64_t>(input.bytes.size(), "rss.input_bytes");
    if (!size || size.value() > limits.max_input_bytes) { reject({ReadStatus::rejected, ReadIssueCode::input_too_large, "RSS input exceeds byte limit", "rss.input_bytes"}); return; }
    const auto& ids = identities_.at(input.input_path);
    if (utf8::find_invalid(input.bytes.begin(), input.bytes.end()) != input.bytes.end()) {
        reject({ReadStatus::rejected, ReadIssueCode::unsupported_encoding, "RSS input is not strict UTF-8", "xml.utf8"}); return;
    }
    auto bytes = input.bytes;
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xef\xbb\xbf") { bytes.remove_prefix(3); }
    std::string parser_bytes;
    if (auto error = preflight(bytes, parser_bytes)) { reject(std::move(*error)); return; }
    if (std::all_of(bytes.begin(), bytes.end(), space)) {
        reject({ReadStatus::no_text, ReadIssueCode::empty_content, "RSS input is empty", "xml.input"}); return;
    }
    if (!parser_bytes.empty()) { bytes = parser_bytes; }
    tinyxml2::XMLDocument document(true, tinyxml2::PRESERVE_WHITESPACE);
    const auto parsed = document.Parse(bytes.data(), bytes.size());
    if (parsed != tinyxml2::XML_SUCCESS) {
        if (parsed == tinyxml2::XML_ELEMENT_DEPTH_EXCEEDED) { reject(unsupported("XML exceeds tinyxml2 internal depth support", "xml.depth")); }
        else { reject(malformed(std::string("XML parse failed: ") + document.ErrorName(), "xml.line: " + std::to_string(document.ErrorLineNum()))); }
        return;
    }
    if (auto error = validate_tree(document, limits_)) { reject(std::move(*error)); return; }
    const auto* rss = document.RootElement();
    if (!rss || rss->NextSiblingElement()) { reject(malformed("XML must have exactly one root element", "xml.root")); return; }
    if (!matches(rss, "", "rss") || !rss->Attribute("version") || std::string_view{rss->Attribute("version")} != "2.0") {
        reject(unsupported("only unnamespaced RSS version 2.0 is supported", "rss.root")); return;
    }
    const auto channel = find_field(rss, "", "channel");
    if (!channel.node || channel.duplicate) { reject(unsupported("RSS requires exactly one channel", "rss.channel")); return; }
    std::size_t count = 0;
    for (auto* item = channel.node->FirstChildElement(); item; item = item->NextSiblingElement()) { if (matches(item, "", "item")) { ++count; } }
    if (count != ids.size()) { throw std::invalid_argument("RSS identity vector must match original item count"); }
    if (count == 0) { reject({ReadStatus::no_text, ReadIssueCode::empty_content, "RSS channel has no items", "rss.channel"}); return; }
    std::size_t ordinal = 0;
    for (auto* item = channel.node->FirstChildElement(); item; item = item->NextSiblingElement()) {
        if (!matches(item, "", "item")) { continue; }
        const auto checked = base::checked_narrow<std::uint64_t>(ordinal, "rss.ordinal");
        if (!checked) { throw std::length_error("RSS item ordinal cannot be represented"); }
        auto result = extract_item(item, input, checked.value(), ids.at(ordinal), limits, limits_);
        if (!sink.emit(std::move(result))) { return; }
        ++ordinal;
    }
}

} // namespace siftwing::document
