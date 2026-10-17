/*
 * PROJECT : SIFTWING
 * FILE    : document_store_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 验证完整区间、实际文件随机往返及 RAII；链接包装只用于短读/EINTR/IO 故障
 */
#include "siftwing/document/document_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <sys/stat.h>
#include <unistd.h>

namespace {
enum class Fault {
    none,
    short_read,
    interrupted,
    eof,
    io
};
Fault fault = Fault::none;
unsigned calls = 0;
bool interrupted = false;
}
extern "C" ssize_t __real_pread(int, void *, std::size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void *buffer, std::size_t size, off_t offset) {
    ++calls;
    if (fault == Fault::interrupted && !interrupted) {
        interrupted = true;
        errno = EINTR;
        return -1;
    }
    if (fault == Fault::eof) {
        return 0;
    }
    if (fault == Fault::io) {
        errno = EIO;
        return -1;
    }
    return __real_pread(
        fd, buffer, fault == Fault::short_read ? std::min(size, std::size_t{5}) : size, offset);
}
namespace {
using namespace siftwing::document;
using siftwing::base::DocumentId;
unsigned checks = 0, failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
DocumentId id(std::uint64_t value) {
    return DocumentId::from_integer(value).value();
}
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
const DocumentStoreLimits limits{{4096, 32768}, 100, 1048576, 4096};
std::size_t descriptors() {
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator{"/proc/self/fd"},
            std::filesystem::directory_iterator{}));
}
struct Temporary final {
    std::string path;
    explicit Temporary(const std::string &parent) {
        std::string pattern = parent + "/document-store-XXXXXX";
        if (!::mkdtemp(pattern.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = std::move(pattern);
    }
    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
void write_file(const std::string &path, const std::string &bytes) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file) {
        throw std::runtime_error("write fixture failed");
    }
    file.close();
    if (!file) {
        throw std::runtime_error("close fixture failed");
    }
}
bool equal(const DocumentRecord &a, const DocumentRecord &b) {
    return a.doc_id == b.doc_id && a.title == b.title && a.content == b.content
           && a.source.kind == b.source.kind
           && a.source.source_id.input_path == b.source.source_id.input_path
           && a.source.source_id.record_ordinal == b.source.source_id.record_ordinal
           && a.source.url == b.source.url && a.source.author == b.source.author
           && a.source.published_at_raw == b.source.published_at_raw;
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            return 2;
        }
        static_assert(!std::is_copy_constructible_v<DocumentStoreReader>);
        static_assert(std::is_nothrow_move_constructible_v<DocumentStoreReader>);
        static_assert(std::is_nothrow_move_assignable_v<DocumentStoreReader>);
        Temporary temporary{argv[1]};
        const auto path = temporary.path + "/library.bin";
        const auto initial_fds = descriptors();
        std::vector<DocumentRecord> records{
            {id(maximum),
                "标题",
                std::string{"a\0b", 3},
                {SourceKind::rss, {"news/a.xml", maximum}, "", std::nullopt, "日期"}},
            {id(0), "", "正文😀", {SourceKind::txt, {"a.txt", 0}, std::nullopt, "", std::nullopt}},
            {id(42),
                "raw",
                "bytes\xff",
                {SourceKind::jsonl, {"history", 9}, "url", "作者", "raw date"}}};
        std::string bytes;
        for (const auto &record : records) {
            bytes += encode_record(record, limits.record).value();
        }
        write_file(path, bytes);
        auto table = build_offset_table(path, limits);
        check(table && table.value().entries.size() == 3
                  && table.value().library_bytes == bytes.size(),
            "scan complete library");
        if (!table) {
            return 1;
        }
        check(table.value().entries[0].doc_id == id(0)
                  && table.value().entries[2].doc_id == id(maximum)
                  && table.value().entries[2].offset == 0,
            "sparse IDs sorted independently of file order");
        auto encoded = encode_offset_table(table.value(), limits);
        check(encoded && encoded.value().size() == 104, "32 byte header and 24 byte triples");
        check(static_cast<bool>(decode_offset_table(encoded.value(), limits)), "table roundtrip");
        for (std::size_t size = 0; size < encoded.value().size(); ++size) {
            check(!decode_offset_table(std::string_view{encoded.value()}.substr(0, size), limits),
                "every table truncation rejected");
        }
        for (std::size_t pos = 0; pos < encoded.value().size(); ++pos) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto broken = encoded.value();
                reinterpret_cast<unsigned char *>(broken.data())[pos] ^=
                    static_cast<unsigned char>(1U << bit);
                check(!decode_offset_table(broken, limits),
                    "every table single-bit mutation rejected");
            }
        }
        for (const auto bad : {DocumentStoreLimits{{0, 32768}, 100, 1048576, 4096},
                 DocumentStoreLimits{{4096, 0}, 100, 1048576, 4096},
                 DocumentStoreLimits{{4096, 32768}, 0, 1048576, 4096},
                 DocumentStoreLimits{{4096, 32768}, 100, 0, 4096},
                 DocumentStoreLimits{{4096, 32768}, 100, 1048576, 0}}) {
            check(!validate_offset_table(table.value(), bad) && !build_offset_table(path, bad)
                      && !DocumentStoreReader::open(path, table.value(), bad),
                "all budgets positive for every consumer");
        }
        const DocumentStoreLimits exact{
            {4096, 32768}, 3, static_cast<std::uint64_t>(bytes.size()), 104};
        check(static_cast<bool>(build_offset_table(path, exact)),
            "exact library/count/table budget inclusive");
        auto smaller = exact;
        smaller.max_table_bytes = 103;
        check(!build_offset_table(path, smaller), "table budget one below exact");
        smaller = exact;
        smaller.max_documents = 2;
        check(!build_offset_table(path, smaller), "count budget stops scan");
        smaller = exact;
        --smaller.max_library_bytes;
        check(!build_offset_table(path, smaller), "library budget before record allocation");
        for (unsigned mutation = 0; mutation < 7; ++mutation) {
            auto bad = table.value();
            if (mutation == 0) {
                bad.entries[1].doc_id = bad.entries[0].doc_id;
            }
            if (mutation == 1) {
                std::swap(bad.entries[0], bad.entries[1]);
            }
            if (mutation == 2) {
                bad.entries[0].offset = maximum;
            }
            if (mutation == 3) {
                bad.entries[0].length = maximum;
            }
            if (mutation == 4) {
                ++bad.entries[0].offset;
            }
            if (mutation == 5) {
                --bad.entries[0].length;
            }
            if (mutation == 6) {
                ++bad.library_bytes;
            }
            check(!validate_offset_table(bad, limits) && !encode_offset_table(bad, limits),
                "invalid order/range/coverage rejects before encoding");
        }
        std::optional<DocumentRecord> surviving;
        {
            auto opened = DocumentStoreReader::open(path, table.value(), limits);
            check(static_cast<bool>(opened), "all records verified before opening reader");
            if (!opened) {
                return 1;
            }
            auto reader = std::move(opened).value();
            table.value().entries.clear();
            check(reader.entries().size() == 3, "reader owns independent table copy");
            for (const auto &record : records) {
                auto read = reader.read(record.doc_id);
                check(read && equal(read.value(), record),
                    "random read all fields exact, including optional empty/NUL/high bytes");
            }
            auto missing = reader.read(id(7));
            check(!missing && missing.error().context == "store.not_found",
                "unknown sparse identity fails");
            surviving = reader.read(id(maximum)).value();
            for (const auto mode : {Fault::short_read, Fault::interrupted, Fault::eof, Fault::io}) {
                fault = mode;
                calls = 0;
                interrupted = false;
                auto read = reader.read(id(maximum));
                if (mode == Fault::short_read) {
                    check(read && equal(read.value(), records[0]) && calls > 1,
                        "pread short reads loop at advanced offsets");
                }
                if (mode == Fault::interrupted) {
                    check(read && equal(read.value(), records[0]) && interrupted && calls >= 2,
                        "EINTR retries same data");
                }
                if (mode == Fault::eof) {
                    check(!read && read.error().context == "store.length",
                        "unexpected EOF with unchanged metadata rejected");
                }
                if (mode == Fault::io) {
                    check(!read && read.error().context == "store.io",
                        "real errno failure returned as Result");
                }
                fault = Fault::none;
            }
            auto moved = std::move(reader);
            check(reader.entries().empty() && !reader.read(id(0))
                      && equal(moved.read(id(0)).value(), records[1]),
                "move transfers fd and closes source logically");
            auto other = std::move(
                DocumentStoreReader::open(path, build_offset_table(path, limits).value(), limits))
                             .value();
            other = std::move(moved);
            auto *self = &other;
            other = std::move(*self);
            check(!moved.read(id(0)) && equal(other.read(id(42)).value(), records[2]),
                "move assignment releases old fd and self move preserves resource");
            std::filesystem::rename(path, path + ".renamed");
            check(equal(other.read(id(0)).value(), records[1]),
                "reader retains opened inode across path rename");
            std::filesystem::remove(path + ".renamed");
            check(equal(other.read(id(maximum)).value(), records[0]),
                "opened file survives unlink while owned fd lives");
        }
        check(surviving && equal(*surviving, records[0]),
            "returned Record independent after reader destruction");
        check(descriptors() == initial_fds, "move/destruction leaves no descriptor leak");
        write_file(path, bytes);
        auto rebuilt = build_offset_table(path, limits);
        {
            auto reader =
                std::move(DocumentStoreReader::open(path, rebuilt.value(), limits)).value();
            auto corrupt = bytes;
            corrupt[corrupt.size() - 1] ^= 1;
            write_file(path, corrupt);
            check(!reader.read(id(42)), "selected record corruption after open rejected");
            check(!DocumentStoreReader::open(path, rebuilt.value(), limits),
                "open validates even nonfirst damaged record");
            write_file(path, bytes + "extra");
            check(
                !reader.read(id(0)), "size growth after open rejected even for unaffected record");
            write_file(path, bytes.substr(0, bytes.size() - 1));
            check(!reader.read(id(maximum)), "truncation after open rejected");
        }
        for (std::size_t size = 1; size < bytes.size(); ++size) {
            if (size == encode_record(records[0], limits.record).value().size()
                || size == bytes.size() - encode_record(records[2], limits.record).value().size()) {
                continue;
            }
            write_file(path, bytes.substr(0, size));
            check(
                !build_offset_table(path, limits), "scanner rejects every partial-record boundary");
        }
        write_file(path, bytes + "tail");
        check(!build_offset_table(path, limits), "trailing library fragment rejected");
        write_file(path,
            encode_record(records[0], limits.record).value()
                + encode_record(records[0], limits.record).value());
        check(!build_offset_table(path, limits), "duplicate library DocId rejected");
        std::string equal_bytes;
        for (auto value : {0U, 1U}) {
            equal_bytes += encode_record({id(value), {}, {}, {}}, limits.record).value();
        }
        write_file(path, equal_bytes);
        auto wrong = build_offset_table(path, limits).value();
        std::swap(wrong.entries[0].offset, wrong.entries[1].offset);
        check(
            validate_offset_table(wrong, limits) && !DocumentStoreReader::open(path, wrong, limits),
            "valid coverage with wrong equal-size record identity rejected");
        write_file(path, "");
        auto empty = build_offset_table(path, limits);
        check(empty && empty.value().entries.empty()
                  && encode_offset_table(empty.value(), limits).value().size() == 32
                  && DocumentStoreReader::open(path, empty.value(), limits),
            "empty store/table valid");
        check(
            !build_offset_table("", limits) && !build_offset_table(std::string{"a\0b", 3}, limits),
            "empty/NUL paths rejected");
        check(!build_offset_table(path + ".missing", limits)
                  && !build_offset_table(temporary.path, limits),
            "missing/nonregular paths rejected");
        std::filesystem::create_symlink(path, path + ".link");
        check(!build_offset_table(path + ".link", limits), "leaf symlink rejected");
        check(::mkfifo((path + ".fifo").c_str(), 0600) == 0
                  && !build_offset_table(path + ".fifo", limits),
            "FIFO rejected without blocking open");
        const auto baseline = descriptors();
        for (unsigned i = 0; i < 32; ++i) {
            (void)DocumentStoreReader::open(path, wrong, limits);
            (void)build_offset_table(path + ".link", limits);
        }
        check(descriptors() == baseline, "failed opens/scans release all descriptors");
    } catch (const std::exception &e) {
        ++failures;
        std::cerr << "unexpected exception: " << e.what() << '\n';
    }
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
