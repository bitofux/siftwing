/*
 * PROJECT : SIFTWING
 * FILE    : simhash_ingestion_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 原创TXT/RSS经过真实管线后生成指纹，验证重复、词序与小改动示例
 */
#include "../support/cppjieba_resources.h"
#include "siftwing/document/rss_reader.h"
#include "siftwing/document/simhash.h"
#include "siftwing/document/txt_reader.h"
#include "siftwing/text/english_tokenizer.h"
#include "siftwing/text/text_pipeline.h"

#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0, failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
} // namespace

int main() {
    try {
        using namespace siftwing::document;
        using namespace siftwing::text;
        const auto id = siftwing::base::DocumentId::from_integer(1).value();
        TxtDocumentReader txt({{"note.txt", {id, "原创"}}});
        RssDocumentReader rss({{"feed.xml", {id}}}, {1000, 64, {4096, 4096, 1000, 1000, 64}});
        const std::string raw = "THE red RED red blue42";
        const std::string xml = "<rss version='2.0'><channel><item><title>原创</title>"
            "<description><![CDATA[<p>THE red RED red blue42</p>]]></description>"
            "</item></channel></rss>";
        auto first = read_documents({{SourceKind::txt, "note.txt", raw}}, txt, {4096,4096,10,4096}, {});
        auto second = read_documents({{SourceKind::rss, "feed.xml", xml}}, rss, {4096,4096,10,4096}, {});
        if (!first || !second || first.value().items.size() != 1 || second.value().items.size() != 1 ||
            !first.value().items[0].record || !second.value().items[0].record) {
            throw std::runtime_error("original adapters did not produce records");
        }
        auto pipeline = TextPipeline::create(std::make_unique<EnglishTokenizer>(),
            StopWords::parse({"the\n"}, {100,10,10,100}).value()).value();
        const TextPipelineLimits limits{{4096,4096}, {4096,100,100,4096},
            {100,4096,100,100,4096,100,4096}};
        const SimHashLimits fingerprint_limits{100,100,4096,100};
        auto a = pipeline->analyze(first.value().items[0].record->content, limits).value();
        auto b = pipeline->analyze(second.value().items[0].record->content, limits).value();
        auto reordered = pipeline->analyze("blue42 RED red red THE", limits).value();
        auto small_change = pipeline->analyze("the red red red green", limits).value();
        auto f = make_simhash(a.tf, fingerprint_limits).value();
        auto same = make_simhash(b.tf, fingerprint_limits).value();
        auto order = make_simhash(reordered.tf, fingerprint_limits).value();
        auto changed = make_simhash(small_change.tf, fingerprint_limits).value();
        check(a.tf.terms == FrequencyTable{{"red",3},{"blue42",1}}, "actual filtered TF features");
        check(f && f->bits == 0x9ec9ba3330a64fb2ULL, "dominant red feature gives fixed hash");
        check(f && same && hamming_distance(*f,*same) == 0, "exact TXT RSS body duplicate");
        check(f && order && hamming_distance(*f,*order) == 0, "same TF with reordered tokens");
        // 此手算样例3票red压过1票修改词，距离0；不把样例外推成所有轻微改动的保证。
        check(f && changed && hamming_distance(*f,*changed) == 0, "one minority word change example");
        auto filtered = pipeline->analyze("THE the", limits).value();
        check(!make_simhash(filtered.tf, fingerprint_limits).value(), "all-filtered document has no fingerprint");
        check(first.value().items[0].record->content == raw &&
            second.value().items[0].record->content == raw, "display document body remains unchanged");
        // 同一指纹入口消费原创中文资源产生的TF，不重新载入分词/IDF/停用资源。
        siftwing::test::Resources resources;
        auto chinese = TextPipeline::create(CppJiebaTokenizer::create(resources.config()).value(),
            StopWords::parse({}, {1,1,1,1}).value()).value();
        auto cn = chinese->analyze("中国 中国 中国 北京", limits).value();
        check(cn.tf.terms == FrequencyTable{{"中国",3},{"北京",1}}, "Chinese pipeline hands complete TF to fingerprint");
        auto cn_f = make_simhash(cn.tf, fingerprint_limits).value();
        check(cn_f && cn_f->bits == 0xb76ab2bc7f31fcf1ULL, "Chinese weighted fingerprint fixed hash");
    } catch (const std::exception &e) {
        ++failures; std::cerr << "unexpected: " << e.what() << '\n';
    }
    std::cout << "simhash ingestion checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
