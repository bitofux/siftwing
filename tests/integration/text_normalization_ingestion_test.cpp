/*
 * PROJECT : SIFTWING
 * FILE    : text_normalization_ingestion_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用原创等价TXT/RSS验证共用规范化入口，保留抽取记录并明确尚未组装文本管线
 */

#include "siftwing/document/rss_reader.h"
#include "siftwing/document/txt_reader.h"
#include "siftwing/text/normalize.h"

#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
unsigned failures = 0;
void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
} // namespace
int main() {
    try {
        using namespace siftwing::document;
        const auto id = siftwing::base::DocumentId::from_integer(1, "test.id").value();
        TxtDocumentReader txt({{"note.txt", {id, "示例 TITLE"}}});
        RssDocumentReader rss({{"feed.xml", {id}}}, {1000, 64, {4096,4096,1000,1000,64}});
        const std::string raw_txt = "Green & BLUE 2026.\r\n中文　123！\r\n";
        const std::string raw_rss = "<rss version='2.0'><channel><item><title>示例 TITLE</title>"
            "<description><![CDATA[<p>Green &amp; BLUE 2026.</p><p>中文　123！</p>]]></description></item></channel></rss>";
        const ReadLimits reading{4096,4096,10,4096};
        auto from_txt = read_documents({{SourceKind::txt,"note.txt",raw_txt}}, txt, reading, {});
        auto from_rss = read_documents({{SourceKind::rss,"feed.xml",raw_rss}}, rss, reading, {});
        check(from_txt && from_rss && from_txt.value().items.size() == 1 && from_rss.value().items.size() == 1,
              "both real byte adapters produce one report from original inputs");
        const auto& first = *from_txt.value().items.at(0).record;
        const auto& second = *from_rss.value().items.at(0).record;
        const std::string first_before = first.content, second_before = second.content;
        auto a = siftwing::text::normalize_utf8(first.content, {4096,4096});
        auto b = siftwing::text::normalize_utf8(second.content, {4096,4096});
        auto query = siftwing::text::normalize_utf8("GREEN & blue 2026.\t中文　123！", {4096,4096});
        check(a && b && query && a.value() == "green & blue 2026. 中文 123！" && a.value() == b.value() && a.value() == query.value(),
              "TXT RSS and direct query-shaped text share one exact independently written expectation");
        check(first.content == first_before && second.content == second_before && first.title == "示例 TITLE" && second.title == first.title,
              "normalization leaves owned source records and presentation text unchanged");
        check(raw_txt.find("\r\n") != std::string::npos && raw_rss.find("<p>") != std::string::npos,
              "input byte strings remain unchanged through adapters and normalization");
        auto title = siftwing::text::normalize_utf8(first.title, {4096,4096});
        check(title && title.value() == "示例 title", "title can be explicitly normalized without overwriting the original");
        auto limited = siftwing::text::normalize_utf8(first.content, {4096,1});
        check(!limited && limited.error().context == "normalize.output_bytes" && first.content == first_before,
              "failed normalization does not mutate document content or publish a partial value");
        // 此测试是显式调用helper的接线验收，不表示M1.11分词或M1.14管线已经存在。
    } catch (const std::exception& e) { ++failures; std::cerr << "FAIL: unexpected exception: " << e.what() << '\n'; }
    std::cout << "Normalization ingestion checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
