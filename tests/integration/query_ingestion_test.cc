/*
 * PROJECT : SIFTWING
 * FILE    : query_ingestion_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 原创TXT/RSS与英文/中文查询复用同一管线并核手写词频
 */
#include "../support/cppjieba_resources.hpp"
#include "siftwing/document/rss_reader.hpp"
#include "siftwing/document/txt_reader.hpp"
#include "siftwing/query/query.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void check(bool condition, const char *label) {
    ++checks; if (!condition) { throw std::runtime_error(label); }
}
} // namespace
int main() {
    try {
        using namespace siftwing;
        const auto id = base::DocumentId::from_integer(7).value();
        document::TxtDocumentReader txt({{"note.txt",{id,"原标题"}}});
        document::RssDocumentReader rss({{"feed.xml",{id}}},{1000,64,{4096,4096,1000,1000,64}});
        const std::string original = "THE 中国北京曙光 RED red 17\r\n";
        const std::string xml = "<rss version='2.0'><channel><item><title>原标题</title>"
            "<description><![CDATA[<p>THE 中国北京曙光 RED red 17</p>]]></description>"
            "</item></channel></rss>";
        const document::ReadLimits reading{4096,4096,10,4096};
        const auto first = document::read_documents({{document::SourceKind::txt,"note.txt",original}},
            txt,reading,{}).value();
        const auto second = document::read_documents({{document::SourceKind::rss,"feed.xml",xml}},
            rss,reading,{}).value();
        check(first.items.size() == 1 && second.items.size() == 1 && first.items[0].record &&
            second.items[0].record, "both adapters deliver owned originals");
        const auto stops = text::StopWords::parse({"the\n"},{100,10,10,100}).value();
        const text::TextPipelineLimits doc{{4096,4096},{4096,100,100,4096},
            {100,4096,100,100,4096,100,4096}};
        const query::QueryLimits budget{{{512,512},{512,50,50,512},{50,512,50,50,512,50,512}},20};
        for (unsigned kind = 0; kind < 2; ++kind) {
            std::unique_ptr<text::TextPipeline> pipeline;
            if (kind == 0) {
                pipeline = text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),stops).value();
            } else {
                // 原创词典/HMM初始化后即销毁；使用者显式选择与建库相同的分词配置。
                test::Resources resources;
                pipeline = text::TextPipeline::create(text::CppJiebaTokenizer::create(resources.config()).value(),stops).value();
            }
            text::FrequencyTable expected{{"17",1},{"red",2}};
            if (kind == 1) { expected.insert({{"中国",1},{"北京",1},{"曙",1},{"光",1}}); }
            const auto a = pipeline->analyze(first.items[0].record->content,doc).value();
            const auto b = pipeline->analyze(second.items[0].record->content,doc).value();
            check(a.tf.terms == expected && b.tf.terms == expected, "hand-written document TF under selected tokenizer");
            for (auto mode : {query::QueryMode::all,query::QueryMode::any}) {
                const auto q = query::Query::from_text("THE 中国北京曙光 red RED 17",*pipeline,{mode,20},budget).value();
                check(q.frequencies().terms == expected && q.frequencies().total_tokens == a.tf.total_tokens,
                    "real query and TXT/RSS share word keys and repeated counts");
                check(q.mode() == mode && q.top_k() == 20, "text analysis preserves requested query options");
            }
            const auto blank_text = pipeline->analyze("THE",doc);
            const auto blank_query = query::Query::from_text("THE",*pipeline,{},budget);
            check(blank_text && blank_text.value().tokens.empty() && !blank_query &&
                blank_query.error().context == "query.empty", "business empty error belongs to Query only");
            auto escaped = query::Query::from_text("red RED",*pipeline,{},budget).value();
            pipeline.reset();
            check(escaped.frequencies().terms == text::FrequencyTable{{"red",2}}, "query survives pipeline and resource destruction");
        }
        check(first.items[0].record->title == "原标题" && second.items[0].record->title == "原标题" &&
            original.find("\r\n") != std::string::npos && xml.find("<p>") != std::string::npos,
            "query creation preserves document/display and original adapter bytes");
        std::cout << "query ingestion checks=" << checks << " failures=0\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
