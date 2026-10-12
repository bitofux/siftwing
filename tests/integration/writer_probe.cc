/*
 * PROJECT : SIFTWING
 * FILE    : writer_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 将原创测试的显式记录/TF和独立推荐频次交给真实Writer，供独立参考读取产物
 */
#include "siftwing/snapshot/writer.hpp"
#include "siftwing/base/checked.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {
using namespace siftwing;
std::uint64_t number() {
    std::string token; if (!(std::cin >> token)) { throw std::runtime_error("missing test number"); }
    std::uint64_t value = 0;
    const auto result = std::from_chars(token.data(),token.data()+token.size(),value);
    if (result.ec != std::errc{} || result.ptr != token.data()+token.size()) { throw std::runtime_error("bad test number"); }
    return value;
}
std::string bytes() {
    std::string token; if (!(std::cin >> token)) { throw std::runtime_error("missing test bytes"); }
    if (token == "-") { return {}; }
    if (token.size()%2) { throw std::runtime_error("bad test hex"); }
    std::string output;
    for (std::size_t i = 0; i < token.size(); i += 2) {
        unsigned value = 0;
        const auto result = std::from_chars(token.data()+i,token.data()+i+2,value,16);
        if (result.ec != std::errc{} || result.ptr != token.data()+i+2) { throw std::runtime_error("bad test hex"); }
        output += static_cast<char>(value);
    }
    return output;
}
text::DocumentTermFrequencies frequencies() {
    text::DocumentTermFrequencies tf;
    const auto count = number(); if (count > 128) { throw std::runtime_error("test term count"); }
    for (std::uint64_t i = 0; i < count; ++i) {
        auto word = bytes(); const auto occurrences = number();
        if (!tf.terms.emplace(std::move(word),occurrences).second) { throw std::runtime_error("duplicate test word"); }
        tf.total_tokens = base::checked_add(tf.total_tokens,occurrences).value();
    }
    return tf;
}
} // namespace
int main(int argc,char **argv) {
    try {
        if (argc != 3) { throw std::runtime_error("parent and version required"); }
        std::uint64_t version = 0; const std::string text_version = argv[2];
        const auto parsed = std::from_chars(text_version.data(),text_version.data()+text_version.size(),version);
        if (parsed.ec != std::errc{} || parsed.ptr != text_version.data()+text_version.size()) { throw std::runtime_error("bad test version"); }
        std::vector<document::DocumentRecord> records; std::vector<index::IndexDocument> documents;
        const auto count = number(); if (count > 128) { throw std::runtime_error("test document count"); }
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto id = number(); auto title = bytes(); auto content = bytes(); auto tf = frequencies();
            const auto identity = base::DocumentId::from_integer(id).value();
            document::DocumentSource source{document::SourceKind::txt,{"original/"+std::to_string(id)+".txt",id%19},
                std::nullopt,std::nullopt,std::nullopt};
            if (id%2) { source.url = "https://original.invalid/"+std::to_string(id); }
            if (id%3 == 0) { source.author = std::string{}; }
            if (id%5 == 0) { source.published_at_raw = "2026-10-11"; }
            records.push_back({identity,std::move(title),std::move(content),std::move(source)});
            documents.push_back({identity,std::move(tf)});
        }
        const auto rec_documents = number(); auto rec_tf = frequencies();
        const text::RecommendationFrequencies rec_frequencies{std::move(rec_tf.terms),rec_tf.total_tokens,rec_documents};
        const index::LexiconLimits ll{100000,10000000,10000,100000,10000000};
        auto raw = index::InvertedIndex::build(documents,{ll,1000,10000000,100000000,1000}).value();
        const auto search = index::WeightedIndex::build(std::move(raw),{1000,100000,100000}).value();
        const auto rec = index::RecommendationDictionary::build(rec_frequencies,{ll,1000,100000000,100000,1000000}).value();
        const snapshot::SnapshotMetadata metadata{base::SnapshotVersion::from_integer(version).value(),"original-build-1",
            {snapshot::sha256_bytes("selected-search:v1").value(),snapshot::sha256_bytes("search-config:v1").value()},
            {snapshot::sha256_bytes("selected-recommendation:v1").value(),snapshot::sha256_bytes("recommendation-config:v1").value()}};
        const snapshot::SnapshotWriteLimits limits{{{100000,1000000},1000,10000000,1000000},{10000,10000000,60000000}};
        const auto result = snapshot::write_snapshot(argv[1],records,search,rec,metadata,limits);
        if (!result) { std::cerr << result.error().context << ": " << result.error().message << '\n'; return 1; }
        if (result.value().durability != snapshot::PublicationDurability::synced) { std::cerr << "unconfirmed publication\n"; return 2; }
        std::cout << "published " << version << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 3; }
}
