/*
 * PROJECT : SIFTWING
 * FILE    : document_store_probe.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 向独立 Python 格式参考暴露建表/打开/读出，仅为测试探针
 */
#include "siftwing/document/document_store.hpp"

#include <iostream>
#include <stdexcept>

namespace {
std::string unhex(const std::string &input) {
    if (input.size() % 2 != 0) {
        throw std::invalid_argument("odd hex");
    }
    std::string bytes;
    for (std::size_t pos = 0; pos < input.size(); pos += 2) {
        const auto byte = static_cast<unsigned char>(std::stoul(input.substr(pos, 2), nullptr, 16));
        bytes.append(reinterpret_cast<const char *>(&byte), 1);
    }
    return bytes;
}
std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : bytes) {
        result += digits[byte >> 4U];
        result += digits[byte & 15U];
    }
    return result;
}
void optional(const std::optional<std::string> &value) {
    if (value) {
        std::cout << '"' << hex(*value) << '"';
    } else {
        std::cout << "null";
    }
}
template <typename T>
int error(const siftwing::base::Result<T> &result) {
    std::cout << result.error().context << '\n';
    return 2;
}
} // namespace
int main(int argc, char **argv) {
    using namespace siftwing::document;
    try {
        if (argc < 3) {
            return 2;
        }
        DocumentStoreLimits limits{{4096, 32768}, 100, 1048576, 4096};
        const std::string mode{argv[1]};
        if (mode == "build" || mode == "validate") {
            if (argc != 3 && argc != 8) {
                return 2;
            }
            if (argc == 8) {
                limits = {{std::stoull(argv[3]), std::stoull(argv[4])},
                    std::stoull(argv[5]),
                    std::stoull(argv[6]),
                    std::stoull(argv[7])};
            }
            auto table = mode == "build" ? build_offset_table(argv[2], limits)
                                         : decode_offset_table(unhex(argv[2]), limits);
            if (!table) {
                return error(table);
            }
            auto bytes = encode_offset_table(table.value(), limits);
            if (!bytes) {
                return error(bytes);
            }
            std::cout << hex(bytes.value()) << '\n';
            return std::cout ? 0 : 1;
        }
        if ((mode != "read" && mode != "inspect") || argc != (mode == "read" ? 5 : 4)) {
            return 2;
        }
        auto table = decode_offset_table(unhex(argv[3]), limits);
        if (!table) {
            return error(table);
        }
        auto reader = DocumentStoreReader::open(argv[2], table.value(), limits);
        if (!reader) {
            return error(reader);
        }
        if (mode == "inspect") {
            std::cout << reader.value().entries().size() << '\n';
            return 0;
        }
        auto id = siftwing::base::DocumentId::from_integer(std::stoull(argv[4]));
        auto record = reader.value().read(id.value());
        if (!record) {
            return error(record);
        }
        const auto &value = record.value();
        std::cout << "{\"id\":" << value.doc_id.value() << ",\"kind\":"
                  << (value.source.kind == SourceKind::txt      ? 1
                         : value.source.kind == SourceKind::rss ? 2
                                                                : 3)
                  << ",\"ordinal\":" << value.source.source_id.record_ordinal << ",\"fields\":[\""
                  << hex(value.title) << "\",\"" << hex(value.content) << "\",\""
                  << hex(value.source.source_id.input_path) << "\",";
        optional(value.source.url);
        std::cout << ',';
        optional(value.source.author);
        std::cout << ',';
        optional(value.source.published_at_raw);
        std::cout << "]}\n";
        return std::cout ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
