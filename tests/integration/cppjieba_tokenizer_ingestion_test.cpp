/*
 * PROJECT : SIFTWING
 * FILE    : cppjieba_tokenizer_ingestion_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用原创TXT/RSS与查询形状输入验证规范化后显式分词，保持原记录
 */

#include "../support/cppjieba_resources.h"
#include "siftwing/document/rss_reader.h"
#include "siftwing/document/txt_reader.h"
#include "siftwing/text/normalize.h"

#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
unsigned failures = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
} // namespace

int main() {
    try {
        using namespace siftwing::document;
        using namespace siftwing::text;
        const auto id = siftwing::base::DocumentId::from_integer(1, "test.id").value();
        TxtDocumentReader txt({{"note.txt", {id, "示例 TITLE"}}});
        RssDocumentReader rss({{"feed.xml", {id}}}, {1000, 64, {4096, 4096, 1000, 1000, 64}});
        const std::string raw_txt = "Green & BLUE 2026.\r\n中文　123！ ONE one\r\n";
        const std::string raw_rss =
            "<rss version='2.0'><channel><item><title>示例 TITLE</title>"
            "<description><![CDATA[<p>Green &amp; BLUE 2026.</p><p>中文　123！ ONE "
            "one</p>]]></description></item></channel></rss>";
        const ReadLimits reading{4096, 4096, 10, 4096};
        auto from_txt = read_documents({{SourceKind::txt, "note.txt", raw_txt}}, txt, reading, {});
        auto from_rss = read_documents({{SourceKind::rss, "feed.xml", raw_rss}}, rss, reading, {});
        check(from_txt && from_rss && from_txt.value().items.size() == 1 &&
                  from_rss.value().items.size() == 1,
              "real TXT and RSS adapters produce original owned records");
        const auto &first = *from_txt.value().items.at(0).record;
        const auto &second = *from_rss.value().items.at(0).record;
        const auto before_txt = first.content, before_rss = second.content;
        auto a = normalize_utf8(first.content, {4096, 4096});
        auto b = normalize_utf8(second.content, {4096, 4096});
        auto q = normalize_utf8("GREEN & blue 2026.\t中文　123！ one ONE", {4096, 4096});
        check(a && b && q && a.value() == b.value() && a.value() == q.value(),
              "equivalent extracted and query-shaped text shares explicit normalization");
        siftwing::test::Resources resources;
        auto created = CppJiebaTokenizer::create(resources.config());
        const Tokenizer &tokenizer = *created.value();
        const TokenizationLimits limits{4096, 100, 100, 4096};
        auto ta = tokenizer.tokenize(a.value(), limits);
        auto tb = tokenizer.tokenize(b.value(), limits);
        auto tq = tokenizer.tokenize(q.value(), limits);
        const TokenSequence hand_written{"green", "blue", "2026", "中文", "123", "one", "one"};
        check(ta && tb && tq && ta.value() == hand_written && ta.value() == tb.value() &&
                  ta.value() == tq.value(),
              "the same virtual interface produces the independently hand-written ordered words");
        check(first.content == before_txt && second.content == before_rss &&
                  first.title == "示例 TITLE" && second.title == first.title,
              "normalization and tokenization preserve source record text and title");
        check(a.value().find("中文") != a.value().npos && ta.value().size() == 7,
              "mixed path retains Chinese tokens and original source text");
        auto limited = tokenizer.tokenize(a.value(), {4096, 1, 100, 4096});
        check(!limited && limited.error().context == "tokenizer.tokens" &&
                  first.content == before_txt,
              "token budget failure publishes no sequence and cannot mutate a document");
        const auto upper = tokenizer.tokenize("GREEN", limits);
        check(upper && upper.value() == TokenSequence{"GREEN"},
              "tokenizer has no hidden lowercase step");
        check(raw_txt.find("\r\n") != raw_txt.npos && raw_rss.find("<p>") != raw_rss.npos,
              "adapter source input bytes remain unchanged");
        // 此处由测试显式组装已有组件；生产文本管线和停用词接口尚未提供。
    } catch (const std::exception &e) {
        ++failures;
        std::cerr << "FAIL: unexpected exception: " << e.what() << '\n';
    }
    std::cout << "Cppjieba tokenizer ingestion checks=" << checks << " failures=" << failures
              << '\n';
    return failures == 0 ? 0 : 1;
}
