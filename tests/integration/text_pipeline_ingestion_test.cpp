/*
 * PROJECT : SIFTWING
 * FILE    : text_pipeline_ingestion_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用原创TXT/RSS与查询黄金证明统一生产文本入口及资源/结果生命周期
 */
#include "../support/cppjieba_resources.h"
#include "siftwing/document/rss_reader.h"
#include "siftwing/document/txt_reader.h"
#include "siftwing/text/english_tokenizer.h"
#include "siftwing/text/text_pipeline.h"

#include <iostream>

namespace {
unsigned checks = 0, failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
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
        const std::string raw_txt = "Green & BLUE 2026.\r\n中文　123！ ONE one\r\n";
        const std::string raw_rss =
            "<rss version='2.0'><channel><item><title>示例 TITLE</title>"
            "<description><![CDATA[<p>Green &amp; BLUE 2026.</p><p>中文　123！ ONE "
            "one</p>]]></description><link>https://example.invalid/a</link></item></channel></rss>";
        TxtDocumentReader txt({{"note.txt", {id, "示例 TITLE"}}});
        RssDocumentReader rss({{"feed.xml", {id}}}, {1000, 64, {4096, 4096, 1000, 1000, 64}});
        const ReadLimits reading{4096, 4096, 10, 4096};
        auto from_txt = read_documents({{SourceKind::txt, "note.txt", raw_txt}}, txt, reading, {});
        auto from_rss = read_documents({{SourceKind::rss, "feed.xml", raw_rss}}, rss, reading, {});
        check(from_txt && from_rss && from_txt.value().items.size() == 1 &&
                  from_rss.value().items.size() == 1 && from_txt.value().items[0].record &&
                  from_rss.value().items[0].record,
              "real adapters produce original owned TXT and RSS records");
        if (!from_txt || !from_rss || !from_txt.value().items[0].record ||
            !from_rss.value().items[0].record) {
            return 1;
        }
        const auto &first = *from_txt.value().items[0].record;
        const auto &second = *from_rss.value().items[0].record;
        const auto before_txt = first.content, before_rss = second.content;
        const TextPipelineLimits document{{4096, 4096}, {4096, 100, 100, 4096},
                                            {100, 4096, 100, 100, 4096, 100, 4096}};
        const TextPipelineLimits query{{512, 512}, {512, 50, 50, 512},
                                         {50, 512, 50, 50, 512, 50, 512}};
        std::string stop_source = "GREEN\n中文\n";
        auto stops = StopWords::parse({stop_source}, {100, 10, 10, 100});
        std::unique_ptr<TextPipeline> pipeline;
        {
            // 原创可手算分词资源只在初始化期读取，随后fixture销毁也不影响管线。
            siftwing::test::Resources resources;
            auto created = CppJiebaTokenizer::create(resources.config());
            if (!created) {
                throw std::runtime_error(created.error().context);
            }
            pipeline = std::move(TextPipeline::create(std::move(created).value(), stops.value())).value();
        }
        stop_source.clear();
        auto a = pipeline->analyze(first.content, document);
        auto b = pipeline->analyze(second.content, document);
        auto q = pipeline->analyze("GREEN & blue 2026.\t中文　123！ one ONE", query);
        const TokenSequence expected{"blue", "2026", "123", "one", "one"};
        const FrequencyTable expected_tf{{"blue", 1}, {"2026", 1}, {"123", 1}, {"one", 2}};
        check(a && b && q && a.value().tokens == expected && b.value().tokens == expected &&
                  q.value().tokens == expected,
              "TXT RSS and query share one pipeline and hand-written ordered filtered words");
        check(a && b && q && a.value().tf.terms == expected_tf && b.value().tf.terms == expected_tf &&
                  q.value().tf.terms == expected_tf && a.value().tf.total_tokens == 5,
              "identical terms and exact repeated TF under different sufficient budgets");
        check(first.content == before_txt && second.content == before_rss &&
                  first.title == "示例 TITLE" && second.title == first.title &&
                  second.source.url == "https://example.invalid/a",
              "pipeline leaves display text title and source metadata intact");
        auto english = std::move(TextPipeline::create(std::make_unique<EnglishTokenizer>(),
                                                       std::move(stops).value())).value();
        auto en = english->analyze(first.content, document);
        check(en && en.value().tokens == expected && en.value().tf.terms == expected_tf,
              "English implementation can be selected explicitly using the same pipeline API");
        // 中文停用过滤前仍消耗分词预算，不能为了最终输出更少而截断早期阶段。
        auto tiny = query;
        tiny.tokenization.max_tokens = 5;
        auto limited = pipeline->analyze(first.content, tiny);
        check(!limited && limited.error().context == "tokenizer.tokens" && first.content == before_txt,
              "unfiltered seven tokens exceed budget even though retained tokens are five");
        auto recovered = pipeline->analyze(first.content, document);
        check(recovered && recovered.value().tokens == expected,
              "failed call does not poison the next pipeline call");
        auto mixed = pipeline->analyze("中国北京曙光 ABC abc", document);
        check(mixed && mixed.value().tokens == TokenSequence{"中国", "北京", "曙", "光", "abc", "abc"},
              "original dictionary boundaries and repeated ASCII words share one entry");
        pipeline.reset();
        check(a && a.value().tokens == expected && a.value().tf.terms == expected_tf,
              "output survives initialized resources and pipeline destruction");
        // 推荐累计由调用者显式选择文档，管线不把查询或调用次数加入累计状态。
        auto cumulative = accumulate_recommendation({}, a.value().tf, {10, 100, 100, 4096, 100});
        auto twice = accumulate_recommendation(cumulative.value(), b.value().tf,
                                               {10, 100, 100, 4096, 100});
        check(twice && twice.value().documents == 2 && twice.value().total_tokens == 10 &&
                  twice.value().terms.at("one") == 4,
              "caller explicitly accumulates document TF without adding the query");
        check(raw_txt.find("\r\n") != raw_txt.npos && raw_rss.find("<p>") != raw_rss.npos,
              "original adapter input bytes retained");
        auto blank = english->analyze("GREEN", query);
        check(blank && blank.value().tokens.empty(), "all-filtered query remains successful at text layer");
    } catch (const std::exception &error) {
        ++failures;
        std::cerr << "unexpected: " << error.what() << '\n';
    }
    std::cout << "text pipeline ingestion checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
