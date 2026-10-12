/*
 * PROJECT : SIFTWING
 * FILE    : roundtrip_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 独立进程加载完整快照，以拥有部件和原来源声明重新发布到新父目录
 */
#include "siftwing/snapshot/reader.hpp"
#include "siftwing/snapshot/writer.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {
std::uint64_t number(const char *input) {
    const std::string_view text{input};
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid test version");
    }
    return value;
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 5) { return 3; }
        using namespace siftwing;
        const snapshot::SnapshotWriteLimits write{{{100000,1000000},1000,10000000,1000000},
            {10000,10000000,60000000}};
        const index::LexiconLimits lexicon{100000,10000000,10000,100000,10000000};
        const snapshot::SnapshotReadLimits read{write.documents,write.manifest,
            {lexicon,1000,10000000,100000000,1000},
            {lexicon,1000,100000000,100000,1000000}};
        const auto input_version = base::SnapshotVersion::from_integer(number(argv[2])).value();
        auto loaded = snapshot::SnapshotReader::open(argv[1],input_version,read);
        if (!loaded) { std::cerr << loaded.error().context << '\n'; return 1; }
        const auto &owner = loaded.value();
        const auto &manifest = *owner.manifest();
        // 仅替换明确输出版本；部件/记录稳定借用到同步Writer返回，来源声明保持。
        const snapshot::SnapshotMetadata metadata{
            base::SnapshotVersion::from_integer(number(argv[4])).value(),manifest.build_version,
            manifest.search_provenance,manifest.recommendation_provenance};
        const auto published = snapshot::write_snapshot(argv[3],*owner.documents(),*owner.search(),
            *owner.recommendation(),metadata,write);
        if (!published) { std::cerr << published.error().context << '\n'; return 1; }
        if (published.value().durability != snapshot::PublicationDurability::synced) { return 2; }
        std::cout << "republished " << metadata.snapshot.value() << '\n';
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 3; }
}
