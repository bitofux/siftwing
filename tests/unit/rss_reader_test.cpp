/*
 * PROJECT : SIFTWING
 * FILE    : rss_reader_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用原创XML及独立文字预期证明RSS字段、来源、停止与失败合同
 */

#include "siftwing/document/rss_reader.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace siftwing::document;
const ReadLimits generous{1024 * 1024, 1024 * 1024, 1000, 8 * 1024 * 1024};
const RssParseLimits parsing{100000, 128, {1024 * 1024, 1024 * 1024, 100000, 100000, 128}};
unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
siftwing::base::DocumentId id(std::uint64_t value) {
    return siftwing::base::DocumentId::from_integer(value, "test.id").value();
}
std::vector<siftwing::base::DocumentId> identities(std::size_t count) {
    std::vector<siftwing::base::DocumentId> result;
    for (std::size_t i = 0; i < count; ++i) { result.push_back(id(static_cast<std::uint64_t>(i + 10))); }
    return result;
}
std::string feed(std::string_view items, std::string_view attributes = "") {
    return "<rss version='2.0' " + std::string(attributes) + "><channel>" + std::string(items) + "</channel></rss>";
}
ReadReport run(std::string_view xml, std::size_t count = 1, ReadLimits limits = generous,
               RssParseLimits config = parsing, ReadPolicy policy = {}, std::string path = "feed.xml") {
    RssDocumentReader reader({{path, identities(count)}}, config);
    auto result = read_documents({{SourceKind::rss, std::move(path), xml}}, reader, limits, policy);
    if (!result) { throw std::runtime_error(result.error().context + ": " + result.error().message); }
    return std::move(result).value();
}
bool issue(const ReadItem& item, ReadIssueCode code, std::string_view region = "") {
    for (const auto& diagnostic : item.issues) {
        if (diagnostic.code == code && (region.empty() || diagnostic.region.find(region) != std::string::npos)) { return true; }
    }
    return false;
}
void problem(const ReadReport& report, ReadStatus status, ReadIssueCode code, const char* label) {
    check(report.items.size() == 1 && report.items[0].status == status && !report.items[0].record && issue(report.items[0], code), label);
}

void selection_and_metadata() {
    // 预期正文手写；不借用被测库再次计算参考，CDATA、XML实体和HTML实体各有独立位置。
    const auto preferred = run(feed("<item><title> T &amp; A </title><description>summary</description>"
        "<content>local full body</content><c:encoded><![CDATA[<p>中文 &amp; A</p><p>B</p>]]></c:encoded>"
        "<link> https://example.invalid/a </link><author> mail </author><d:creator> 作者 </d:creator>"
        "<pubDate> raw unparsed date </pubDate><category>a</category><category>b</category></item>",
        "xmlns:c='http://purl.org/rss/1.0/modules/content/' xmlns:d='http://purl.org/dc/elements/1.1/'"));
    const auto& item = preferred.items.at(0);
    check(item.status == ReadStatus::success && item.issues.empty(), "preferred body and present title need no warning");
    check(item.record->title == "T & A" && item.record->content == "中文 & A\nB", "XML title and HTML body have distinct decoding rules");
    check(item.record->source.url == "https://example.invalid/a" && item.record->source.author == "作者" &&
          item.record->source.published_at_raw == "raw unparsed date", "dc creator wins and optional metadata stays separate raw text");
    check(item.record->source.kind == SourceKind::rss && item.record->doc_id.value() == 10 &&
          item.source_id.record_ordinal == 0 && item.record->source.source_id.input_path == "feed.xml", "explicit ID and original source position are retained");
    const auto local = run(feed("<item><title>T</title><content><![CDATA[<p>Full</p>]]></content><description>Summary</description></item>"));
    check(local.items[0].record->content == "Full" && issue(local.items[0], ReadIssueCode::missing_metadata, "/content"), "observed local content wins over description summary and records fallback");
    const auto description = run(feed("<item><title>T</title><description>&lt;p&gt;Green &amp;amp; blue&lt;/p&gt;</description></item>"));
    check(description.items[0].record->content == "Green & blue" && issue(description.items[0], ReadIssueCode::missing_metadata, "description"), "description undergoes XML then HTML decoding exactly once each");
    const auto mixed = run(feed("<item><title>A<![CDATA[B]]><!--c-->C</title><content>A<![CDATA[ &amp; ]]><!--c-->B</content></item>"));
    check(mixed.items[0].record->title == "ABC" && mixed.items[0].record->content == "A & B", "all direct Text/CDATA pieces are concatenated, unlike GetText alone");
    const auto plain_title = run(feed("<item><title>&lt;b&gt;T&lt;/b&gt;</title><description>body</description><link/><author/><pubDate/></item>"));
    check(plain_title.items[0].record->title == "<b>T</b>", "XML title is plain text and is not reinterpreted as HTML");
    const auto& meta = plain_title.items[0].record->source;
    check(meta.url && meta.url->empty() && meta.author && meta.author->empty() && meta.published_at_raw && meta.published_at_raw->empty(), "provided empty optional metadata differs from absence");
    const auto absent = run(feed("<item><title>T</title><description>body</description><source><a>ignored</a></source></item>"));
    check(!absent.items[0].record->source.url && !absent.items[0].record->source.author && !absent.items[0].record->source.published_at_raw, "unrecognized nested extension is not fabricated into metadata");
    const auto recovered = run(feed("<item><title>T</title><content><![CDATA[<p>A</bogus>B]]></content></item>"));
    check(recovered.items[0].record->content == "AB" && issue(recovered.items[0], ReadIssueCode::malformed_input, "content"), "HTML recovery remains a warning on the selected body");
    for (const auto& empty : std::vector<std::string>{"", " ", "<p></p>", "<script>x</script>", "<pre> </pre>"}) {
        const auto fallback = run(feed("<item><title>T</title><c:encoded><![CDATA[" + empty + "]]></c:encoded><content/><description>fallback</description></item>",
                                       "xmlns:c='http://purl.org/rss/1.0/modules/content/'"));
        check(fallback.items[0].record->content == "fallback" && issue(fallback.items[0], ReadIssueCode::missing_metadata, "description"), "only missing or no-text preferred fields permit fallback");
    }
    const auto no_decode_twice = run(feed("<item><title>T</title><description><![CDATA[&amp;lt;p&amp;gt;]]></description></item>"));
    check(no_decode_twice.items[0].record->content == "&lt;p&gt;", "HTML entity output is not recursively parsed again");
}

void namespaces_and_format() {
    const auto alias = run(feed("<item><title>T</title><encoded xmlns='http://purl.org/rss/1.0/modules/content/'>full</encoded><description>short</description></item>"));
    check(alias.items[0].record->content == "full" && alias.items[0].status == ReadStatus::success, "content URI works with default namespace on field");
    const auto spoof = run(feed("<item><title>T</title><content:encoded>spoof</content:encoded><description>real</description></item>", "xmlns:content='urn:wrong'"));
    check(spoof.items[0].record->content == "real", "bare prefix spelling does not grant content namespace semantics");
    const auto shadow = run(feed("<item><title>T</title><q:encoded xmlns:q='http://purl.org/rss/1.0/modules/content/'>bound</q:encoded></item>", "xmlns:q='urn:outer'"));
    check(shadow.items[0].record->content == "bound", "nearest ancestor namespace declaration controls field identity");
    for (const auto& invalid : std::vector<std::string>{
            "<item><p:x/></item>", "<item xmlns:xml='urn:wrong'/>", "<item xmlns:p=''/>",
            "<item xmlns:p='http://www.w3.org/2000/xmlns/'/>", "<item xmlns='http://www.w3.org/XML/1998/namespace'/>",
            "<item xmlns:a='urn:same' xmlns:b='urn:same' a:n='1' b:n='2'/>", "<item><a:b:c/></item>", "<item xmlns:a='urn:a'><a:1/></item>", "<item><·a/></item>"}) {
        const auto report = run(feed(invalid));
        problem(report, ReadStatus::failed, ReadIssueCode::malformed_input, "invalid namespace binding rejects whole XML before item output");
    }
    for (const auto& xml : std::vector<std::string>{"<feed/>", "<rss><channel/></rss>", "<rss version='1.0'><channel/></rss>",
            "<rss version='2.0' xmlns='urn:rss'><channel/></rss>", "<rss version='2.0'/>", "<rss version='2.0'><channel/><channel/></rss>"}) {
        problem(run(xml, 0), ReadStatus::rejected, ReadIssueCode::unsupported_format, "unsupported XML shape is not guessed to be RSS2");
    }
    problem(run("<rss/><rss/>", 0), ReadStatus::failed, ReadIssueCode::malformed_input, "multiple XML roots fail instead of silently using first");
    const auto pi = run("<?before data?>" + feed("<item><title>A<?inside ignored?>B</title><description>x</description><é·/></item>") + "<?after data?>");
    check(pi.items[0].record->title == "AB", "legal XML PI is ignored in fields and around root; Unicode XML names remain valid");
    const auto comments = run("<!-- <!DOCTYPE harmless> -->" + feed("<item><title>T</title><description><![CDATA[<pre>&lt;!DOCTYPE demo&gt;</pre>]]></description></item>"));
    check(comments.items[0].record->content == "<!DOCTYPE demo>", "DOCTYPE-looking text in comments/CDATA is not a declaration");
    for (const auto& field : std::vector<std::string>{"<title>T</title><title>U</title><description>x</description>",
            "<title>T</title><description>x</description><description>y</description>",
            "<title>T</title><description><p>not CDATA</p></description>"}) {
        problem(run(feed("<item>" + field + "</item>")), ReadStatus::rejected, ReadIssueCode::unsupported_format, "duplicate or consumed nested fields do not silently discard data");
    }
}

void invalid_xml_and_empty() {
    for (const auto& xml : std::vector<std::string>{"", " \t\r\n", "\xef\xbb\xbf"}) {
        problem(run(xml, 0), ReadStatus::no_text, ReadIssueCode::empty_content, "empty XML input is explicit no_text");
    }
    problem(run(feed(""), 0), ReadStatus::no_text, ReadIssueCode::empty_content, "empty channel is explicit no_text");
    for (const auto& item : std::vector<std::string>{"<item/>", "<item><title>T</title></item>", "<item><description><![CDATA[<p></p>]]></description></item>"}) {
        problem(run(feed(item)), ReadStatus::no_text, ReadIssueCode::empty_content, "missing or filtered-empty body creates no document");
    }
    const auto valid = feed("<item><title>T</title><description>x</description></item>");
    check(run("\xef\xbb\xbf<?xml version='1.0' encoding='uTf-8' standalone='yes'?>" + valid).items[0].record->content == "x", "UTF-8 BOM and explicit XML1.0 declaration are supported");
    for (const auto& bad : std::vector<std::string>{"&unknown;", "&#0;", "&#xD800;", "&#1114112;", "&#-1;", "&#x;", "&#xFFFFFFFFFFFFFFFF;", "&amp", "]]>", std::string(1, '\0'), std::string(1, '\1')}) {
        problem(run(feed("<item><description>" + bad + "</description></item>")), ReadStatus::failed, ReadIssueCode::malformed_input, "forbidden XML characters or entity spelling fail without substitution");
    }
    for (const auto& bad : std::vector<std::string>{"\xff", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"}) {
        problem(run(bad, 0), ReadStatus::rejected, ReadIssueCode::unsupported_encoding, "invalid UTF-8 is rejected before XML parsing");
    }
    for (const auto& prefix : std::vector<std::string>{"<!DOCTYPE rss>", "<!DOCTYPE rss [<!ENTITY x SYSTEM 'file:///not-accessed'>]>", "<!ENTITY x 'value'>"}) {
        problem(run(prefix + valid), ReadStatus::rejected, ReadIssueCode::unsupported_format, "DTD and entity declarations are rejected without external resource loading");
    }
    problem(run("<?xml version='1.0' encoding='GBK'?>" + valid), ReadStatus::rejected, ReadIssueCode::unsupported_encoding, "non-UTF8 declared encoding is not guessed or converted");
    problem(run("<?xml version='1.1'?>" + valid), ReadStatus::rejected, ReadIssueCode::unsupported_format, "XML1.1 is outside the declared profile");
    for (const auto& xml : std::vector<std::string>{"<?xml?>" + valid, "<?xml encoding='UTF-8'?>" + valid,
            "<?xml version='1.0' version='1.0'?>" + valid, " <?xml version='1.0'?>" + valid,
            "<!--bad--comment-->" + valid, "<!--bad--->" + valid, "<?1bad?>" + valid,
            "<rss version='2.0'a='b'><channel/></rss>", "<rss version='2.0'><channel><item></channel></rss>",
            "<rss version='2.0'><channel/></rss>trailing", "<rss version='2.0' a='<bad'><channel/></rss>"}) {
        problem(run(xml), ReadStatus::failed, ReadIssueCode::malformed_input, "malformed declarations comments tags or attributes fail as XML");
    }
    const auto malformed_tail = run("<rss version='2.0'><channel><item><description>valid</description></item><item></channel></rss>", 2);
    check(malformed_tail.items.size() == 1 && malformed_tail.items[0].status == ReadStatus::failed && !malformed_tail.items[0].record, "bad XML tail never publishes earlier item as complete");
}

void titles_and_budgets() {
    const auto body = "<item><description>x</description></item>";
    for (const auto& path : std::vector<std::string>{"dir/name.part.xml", "dir/.xml", "dir/README"}) {
        const auto result = run(feed(body), 1, generous, parsing, {}, path);
        const auto expected = path == "dir/name.part.xml" ? "name.part#0" : path == "dir/.xml" ? ".xml#0" : "README#0";
        check(result.items[0].record->title == expected && issue(result.items[0], ReadIssueCode::title_fallback), "title fallback uses stem and original zero-based ordinal");
    }
    const auto blank = run(feed("<item><title> \t　 </title><description>x</description></item>"));
    check(blank.items[0].record->title == "feed#0", "Unicode whitespace-only title falls back");
    const auto trim_title = run(feed("<item><title> 　A  B　 </title><description>x</description></item>"));
    check(trim_title.items[0].record->title == "A  B", "title trims only outer Unicode whitespace");
    problem(run(feed(body), 1, generous, parsing, {}, std::string("\xff.xml")), ReadStatus::rejected, ReadIssueCode::malformed_input, "invalid fallback filename is not copied into output title");
    const auto simple = feed("<item><title>T</title><description>ABC</description></item>");
    const auto exact = run(simple, 1, {static_cast<std::uint64_t>(simple.size()), 4, 1, 4});
    check(exact.items[0].record->content == "ABC" && exact.total_text_bytes == 4, "exact input/title-plus-body/record/total limits are inclusive");
    problem(run(simple, 1, {static_cast<std::uint64_t>(simple.size() - 1), 4, 1, 4}), ReadStatus::rejected, ReadIssueCode::input_too_large, "input byte budget rejects before parsing");
    problem(run(simple, 1, {4096, 3, 1, 4}), ReadStatus::rejected, ReadIssueCode::document_too_large, "title participates in output budget and no truncated document escapes");
    const auto total = run(simple, 1, {4096, 4, 1, 3});
    check(total.items.empty() && total.stop && total.stop->reason == ReadStopReason::total_text_limit, "common total text budget rejects candidate without partial payload");
    auto config = parsing;
    config.max_xml_nodes = 2; config.max_xml_depth = 2;
    check(run("<rss version='2.0'><channel/></rss>", 0, generous, config).counts.no_text == 1, "exact XML node/depth bounds include root and channel");
    config.max_xml_nodes = 1;
    const auto nodes = run("<rss version='2.0'><channel/></rss>", 0, generous, config);
    check(nodes.counts.rejected == 1 && issue(nodes.items[0], ReadIssueCode::unsupported_format, "xml.nodes"), "XML node budget is a located support-range rejection");
    config = parsing; config.max_xml_depth = 1;
    const auto depth = run(simple, 1, generous, config);
    check(depth.counts.rejected == 1 && issue(depth.items[0], ReadIssueCode::unsupported_format, "xml.depth"), "XML depth budget is checked before any item output");
    config = parsing; config.max_xml_nodes = 2;
    problem(run("<!--counted-->" + feed(""), 0, generous, config), ReadStatus::rejected, ReadIssueCode::unsupported_format, "ignored comments still count towards XML structural budget");
    const auto preferred = feed("<item><title>T</title><c:encoded><![CDATA[<p>AB</p>]]></c:encoded><description>fallback</description></item>", "xmlns:c='http://purl.org/rss/1.0/modules/content/'");
    config = parsing; config.html.max_input_bytes = 8;
    const auto input = run(preferred, 1, generous, config);
    check(input.counts.rejected == 1 && issue(input.items[0], ReadIssueCode::input_too_large, "content:encoded"), "preferred field byte overflow rejects rather than selecting summary");
    config = parsing; config.html.max_output_bytes = 1;
    problem(run(preferred, 1, generous, config), ReadStatus::rejected, ReadIssueCode::document_too_large, "preferred output overflow cannot be hidden by fallback");
    for (unsigned budget = 0; budget < 3; ++budget) {
        config = parsing;
        if (budget == 0) { config.html.max_tokens = 1; }
        else if (budget == 1) { config.html.max_nodes = 1; }
        else { config.html.max_depth = 1; }
        const auto rejected = run(preferred, 1, generous, config);
        check(rejected.counts.rejected == 1 && issue(rejected.items[0], ReadIssueCode::unsupported_format, "html."), "preferred HTML structural budget refusal is located and no fallback occurs");
    }
}

class StopSink final : public ReadSink {
public:
    unsigned calls = 0;
    bool accept(ReadItem) override { ++calls; return false; }
};

void identities_stop_and_ownership() {
    const auto xml = feed("<item/><item><title>T</title><description>x</description></item>");
    RssDocumentReader reader({{"feed.xml", {id(0), id(std::numeric_limits<std::uint64_t>::max())}}}, parsing);
    for (unsigned repetition = 0; repetition < 20; ++repetition) {
        auto result = read_documents({{SourceKind::rss, "feed.xml", xml}}, reader, generous, {});
        check(result && result.value().items.size() == 2 && result.value().items[0].status == ReadStatus::no_text &&
              result.value().items[1].source_id.record_ordinal == 1 && result.value().items[1].record->doc_id.value() == std::numeric_limits<std::uint64_t>::max(),
              "no_text does not consume different identity positions and repeated reads reset state");
    }
    StopSink sink;
    reader.read({SourceKind::rss, "feed.xml", xml}, generous, sink);
    check(sink.calls == 1, "sink false stops before processing next item");
    RssDocumentReader wrong({{"feed.xml", {id(1)}}}, parsing);
    StopSink mismatch;
    bool thrown = false;
    try { wrong.read({SourceKind::rss, "feed.xml", xml}, generous, mismatch); } catch (const std::invalid_argument&) { thrown = true; }
    check(thrown && mismatch.calls == 0, "ID cardinality mismatch is caller error before any emit");
    thrown = false;
    try { reader.read({SourceKind::rss, "missing.xml", xml}, generous, mismatch); } catch (const std::out_of_range&) { thrown = true; }
    check(thrown && mismatch.calls == 0, "missing ID configuration is not misreported as input parse failure");
    for (unsigned field = 0; field < 7; ++field) {
        auto config = parsing;
        switch (field) {
        case 0: config.max_xml_nodes = 0; break; case 1: config.max_xml_depth = 0; break;
        case 2: config.html.max_input_bytes = 0; break; case 3: config.html.max_output_bytes = 0; break;
        case 4: config.html.max_tokens = 0; break; case 5: config.html.max_nodes = 0; break;
        default: config.html.max_depth = 0; break;
        }
        thrown = false;
        try { RssDocumentReader zero({}, config); } catch (const std::invalid_argument&) { thrown = true; }
        check(thrown, "all seven RSS parse budgets must be positive");
    }
    for (unsigned field = 0; field < 4; ++field) {
        auto zero = generous;
        if (field == 0) { zero.max_input_bytes = 0; } else if (field == 1) { zero.max_document_bytes = 0; }
        else if (field == 2) { zero.max_records = 0; } else { zero.max_total_text_bytes = 0; }
        thrown = false;
        try { reader.read({SourceKind::rss, "feed.xml", xml}, zero, mismatch); } catch (const std::invalid_argument&) { thrown = true; }
        check(thrown && mismatch.calls == 0, "direct read validates zero acceptance budgets before emit");
    }
    const auto unsupported = read_documents({{SourceKind::txt, "unconfigured.txt", "x"}}, reader, generous, {});
    check(unsupported && unsupported.value().counts.rejected == 1 && issue(unsupported.value().items[0], ReadIssueCode::unsupported_format), "wrong kind is refused before accessing identity configuration");
    const auto mixed = feed("<item><description><p>unsupported XML child</p></description></item><item><title>T</title><description>second</description></item>");
    const auto stopped = run(mixed, 2);
    check(stopped.items.size() == 1 && stopped.stop && stopped.stop->reason == ReadStopReason::failure_policy, "common default failure policy stops at rejected item");
    const auto continued = run(mixed, 2, generous, parsing, {FailurePolicy::continue_reading, PartialPolicy::reject});
    check(continued.items.size() == 2 && continued.items[0].status == ReadStatus::rejected && continued.items[1].record->content == "second" &&
          continued.items[1].source_id.record_ordinal == 1, "explicit continuation retains failure and original next source ordinal");
    const auto limited = run(xml, 2, {4096, 4096, 1, 8192});
    check(limited.items.size() == 1 && limited.stop && limited.stop->reason == ReadStopReason::record_limit && limited.stop->source_id.record_ordinal == 1, "record budget counts no_text and reports unaccepted next candidate");
    RssDocumentReader duplicate({{"feed.xml", {id(5), id(5)}}}, parsing);
    const auto duplicate_xml = feed("<item><title>A</title><description>x</description></item><item><title>B</title><description>y</description></item>");
    check(!read_documents({{SourceKind::rss, "feed.xml", duplicate_xml}}, duplicate, generous, {}), "collector rejects duplicate accepted IDs rather than renumbering");
    ReadReport owned;
    {
        std::string bytes = feed("<item><title>标题</title><description><![CDATA[<p>拥有正文</p>]]></description><author>作者</author></item>");
        auto config = std::map<std::string, std::vector<siftwing::base::DocumentId>>{{"feed.xml", {id(42)}}};
        RssDocumentReader local(config, parsing); config.at("feed.xml")[0] = id(99);
        auto result = read_documents({{SourceKind::rss, "feed.xml", bytes}}, local, generous, {});
        owned = std::move(result).value(); bytes.assign(bytes.size(), 'x');
    }
    check(owned.items[0].record->title == "标题" && owned.items[0].record->content == "拥有正文" &&
          owned.items[0].record->source.author == "作者" && owned.items[0].record->doc_id.value() == 42,
          "report and identity config own their storage after caller bytes DOM reader and original map destruction");
}
} // namespace

int main() {
    try { selection_and_metadata(); namespaces_and_format(); invalid_xml_and_empty(); titles_and_budgets(); identities_stop_and_ownership(); }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL: unexpected exception: " << error.what() << '\n'; }
    std::cout << "RSS reader checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
