/*
 * PROJECT : SIFTWING
 * FILE    : v1_lifecycle_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 同进程反复完整加载、查询与失败，观察fd交接和拥有结果生命
 * IMPLEMENTATION : 独立驱动提供真实应用快照；显式检查不受NDEBUG影响，观察器不常驻fd。
 */
#include "siftwing/snapshot/reader.hpp"
#include "siftwing/query/top_k.hpp"
#include "siftwing/query/recommendation.hpp"
#include "siftwing/query/summary.hpp"
#include "siftwing/query/highlight.hpp"
#include "siftwing/text/english_tokenizer.hpp"
#include "siftwing/text/cppjieba_tokenizer.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace siftwing;
unsigned checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        throw std::runtime_error(message);
    }
}
template <class T>
T take(base::Result<T> value) {
    if (!value) {
        throw std::runtime_error(value.error().context + ": " + value.error().message);
    }
    return std::move(value).value();
}
// /proc目录枚举自身的观察fd只在本函数内存在；每次同样计入，返回前迭代器关闭。
std::size_t descriptors() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
const snapshot::SnapshotReadLimits limits{{{1048576, 1048576}, 100, 1048576, 1048576},
    {65536, 1048576, 6291456},
    {{1000, 1048576, 4096, 1000, 1048576}, 100, 10000, 100000, 100},
    {{1000, 1048576, 4096, 1000, 1048576}, 100, 100000, 1000, 10000}};
const text::TextPipelineLimits text_limits{
    {1024, 1024}, {1024, 100, 100, 1024}, {100, 1024, 100, 100, 1024, 100, 1024}};
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 5) {
            return 2;
        }
        const auto version = take(base::SnapshotVersion::from_integer(0));
        const auto baseline = descriptors();
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            std::string survived;
            text::TokenSequence tokens;
            {
                auto loaded = take(snapshot::SnapshotReader::open(argv[1], version, limits));
                check(descriptors() == baseline,
                    "successful Reader retains no fd before destruction");
                auto reader = std::move(loaded);
                check(!loaded.manifest() && reader.manifest(),
                    "move transfers sole Reader ownership");
                auto pipeline =
                    take(text::TextPipeline::create(std::make_unique<text::EnglishTokenizer>(),
                        take(text::StopWords::parse({}, {1024, 100, 100, 1024}))));
                const auto q =
                    take(query::Query::from_text("lantern", *pipeline, {}, {text_limits, 100}));
                const auto ranked = take(
                    query::rank_top_k(q, *reader.search(), {{{100, 10000, 100}, 100000}, 100}));
                check(!ranked.empty(), "valid snapshot has golden lantern results");
                const auto *record = reader.document(ranked.front().document_id);
                check(record != nullptr, "original identity resolves in same Reader");
                const auto summary = take(query::make_summary(
                    q, record->content, {160, 40}, {1048576, 100, 1024, 100000, 4096}));
                const auto highlight = take(query::make_highlight(
                    q, summary.text, {1048576, 100, 1024, 100000, 1000, 1048576}));
                check(highlight.html.find("<mark>") != std::string::npos,
                    "display chain marks golden term");
                const auto suggestions = take(query::recommend_terms("lantern",
                    *reader.recommendation(),
                    10,
                    {1024, 100, 100, 10000, 1000, 1048576, 100000, 1048576}));
                check(!suggestions.empty() && suggestions.front().term == "lantern",
                    "independent recommendation chain");
                survived = highlight.html;
                auto chinese = take(text::CppJiebaTokenizer::create(
                    {argv[3], argv[4], std::nullopt, 1048576, false}));
                tokens = take(chinese->tokenize("铜壶 灯笼", {1024, 100, 100, 1024}));
                check(
                    descriptors() == baseline, "resource factory and complete query retain no fd");
            }
            check(survived.find("<mark>") != std::string::npos
                      && tokens == text::TokenSequence{"铜壶", "灯笼"},
                "display and tokens own bytes after Reader/pipeline/factory destruction");
            check(descriptors() == baseline, "all successful owners destroyed without fd growth");
            {
                const auto rejected = snapshot::SnapshotReader::open(argv[2], version, limits);
                check(!rejected, "checksum-valid semantic corruption rejected");
                check(descriptors() == baseline,
                    "failed Reader closes acquired descriptors before Result dies");
                const auto absent = snapshot::SnapshotReader::open(
                    std::string{argv[1]} + "/absent", version, limits);
                check(!absent, "missing directory rejected");
                const auto failed = text::CppJiebaTokenizer::create(
                    {argv[3], std::string{argv[4]} + ".absent", std::nullopt, 1048576, false});
                check(!failed, "resource factory failure after dictionary read");
                check(descriptors() == baseline,
                    "all failed resource acquisition closes descriptors");
            }
            check(descriptors() == baseline, "failed Result destruction preserves fd baseline");
        }
        std::cout << "iterations=32 checks=" << checks << " fd_baseline=" << baseline
                  << " fd_final=" << descriptors() << " failures=0\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
