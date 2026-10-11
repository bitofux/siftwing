/*
 * PROJECT : SIFTWING
 * FILE    : record_format_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 为独立 Python 字节参考提供显式测试探针，不是生产 CLI 或文件发布入口
 */
#include "siftwing/document/record_format.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
std::string unhex(const std::string &text) {
    if (text.size() % 2 != 0) { throw std::invalid_argument("odd hex"); }
    std::string result;
    for (std::size_t pos = 0; pos < text.size(); pos += 2) {
        const auto number = std::stoul(text.substr(pos, 2), nullptr, 16);
        const auto byte = static_cast<unsigned char>(number);
        result.append(reinterpret_cast<const char *>(&byte), 1);
    }
    return result;
}
void hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char byte : bytes) { std::cout << digits[byte >> 4U] << digits[byte & 15U]; }
    std::cout << '\n';
}
} // namespace
int main(int argc, char **argv) {
    using namespace siftwing::document;
    try {
        if (argc == 5 && std::string{argv[1]} == "validate") {
            auto value = validate_record(unhex(argv[4]), {std::stoull(argv[2]), std::stoull(argv[3])});
            if (!value) { std::cout << value.error().context << '\n'; return 2; }
            std::cout << value.value().doc_id.value() << ' ' << value.value().byte_size << '\n';
            return 0;
        }
        if (argc != 12 && argc != 13) { return 2; }
        const std::string mode{argv[1]};
        if (mode != "encode" && mode != "write") { return 2; }
        const auto id = siftwing::base::DocumentId::from_integer(std::stoull(argv[2]));
        const auto kind = std::stoul(argv[4]);
        const auto flags = std::stoul(argv[5]);
        if (kind < 1 || kind > 3 || flags > 7) { return 2; }
        DocumentRecord record{id.value(), unhex(argv[6]), unhex(argv[7]),
            {kind == 1 ? SourceKind::txt : kind == 2 ? SourceKind::rss : SourceKind::jsonl,
             {unhex(argv[8]), std::stoull(argv[3])}, std::nullopt, std::nullopt, std::nullopt}};
        if (flags & 1U) { record.source.url = unhex(argv[9]); }
        if (flags & 2U) { record.source.author = unhex(argv[10]); }
        if (flags & 4U) { record.source.published_at_raw = unhex(argv[11]); }
        const RecordFormatLimits limits{65536, 1048576};
        if (mode == "write") {
            if (argc != 13) { return 2; }
            // 路径仅来自测试拥有的临时目录；flush/close 由这个调用方承担，失败不报告成功。
            std::ofstream file{argv[12], std::ios::binary | std::ios::trunc};
            const auto result = write_record(file, record, limits);
            if (!result) { return 2; }
            file.flush();
            if (!file) { return 2; }
            file.close();
            return file ? 0 : 2;
        }
        auto bytes = encode_record(record, limits);
        if (!bytes) { return 2; }
        std::ostringstream output{std::ios::binary | std::ios::out};
        const auto result = write_record(output, record, limits);
        if (!result || output.str() != bytes.value()) { return 1; }
        hex(bytes.value());
        return std::cout ? 0 : 1;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
