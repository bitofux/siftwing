/*
 * PROJECT : SIFTWING
 * FILE    : writer_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 验证发布提交点、预算/身份/路径、IO失败残留及fd释放，Release仍执行
 */
#include "siftwing/snapshot/writer.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace siftwing;
using namespace snapshot;
enum class Fault {
    none,
    interruptions,
    short_io,
    zero_write,
    write_error,
    eof,
    read_error,
    corruption,
    file_sync,
    directory_sync,
    parent_sync,
    write_close,
    read_close,
    directory_close,
    parent_close,
    random_error,
    random_zero,
    collisions,
    mkdir_error,
    create_open,
    reopen,
    temp_open,
    stat,
    directory_stream,
    extra,
    missing,
    rename,
    unsupported,
    target_race,
    allocation
};
struct State {
    bool active = false, renamed = false, ready_at_rename = false;
    Fault fault = Fault::none;
    unsigned hits = 0, write_calls = 0, read_calls = 0, random_calls = 0, sync_calls = 0,
             open_calls = 0, write_opens = 0, read_opens = 0, write_closes = 0, read_closes = 0,
             file_syncs = 0, directory_syncs = 0, renames = 0;
    int complete_directory = -1;
    ino_t raced_inode = 0;
} state;
unsigned checks = 0, failures = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << message << '\n';
    }
}
std::string load(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read test output");
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
std::vector<fs::path> entries(const fs::path &parent) {
    std::vector<fs::path> paths;
    for (const auto &entry : fs::directory_iterator(parent)) {
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}
std::size_t fd_count() {
    return entries("/proc/self/fd").size();
}
struct Fixture {
    std::vector<document::DocumentRecord> records;
    index::WeightedIndex search;
    index::RecommendationDictionary recommendation;
};
Fixture fixture(bool empty = false) {
    std::vector<index::IndexDocument> input;
    std::vector<document::DocumentRecord> records;
    if (!empty) {
        const auto zero = base::DocumentId::from_integer(0).value();
        const auto seven = base::DocumentId::from_integer(7).value();
        const auto last =
            base::DocumentId::from_integer(std::numeric_limits<std::uint64_t>::max()).value();
        input = {{last, {{{"blue", 1}, {"red", 1}}, 2}},
            {zero, {{}, 0}},
            {seven, {{{"red", 2}, {"蓝😀", 1}}, 3}}};
        records = {{last, "末篇", "blue red", {}},
            {zero, "", "", {}},
            {seven,
                "原创标题",
                std::string{"red\0red", 7},
                {document::SourceKind::txt,
                    {"original.txt", 4},
                    std::string{},
                    std::nullopt,
                    std::string{"2026"}}}};
    }
    auto raw = index::InvertedIndex::build(
        input, {{1000, 10000, 1000, 1000, 10000}, 100, 10000, 100000, 100})
                   .value();
    auto weighted = index::WeightedIndex::build(std::move(raw), {100, 1000, 10000}).value();
    text::RecommendationFrequencies frequencies;
    if (!empty) {
        frequencies = {{{"blue", 1}, {"red", 8}, {"蓝😀", 3}}, 12, 5};
    }
    auto rec = index::RecommendationDictionary::build(
        frequencies, {{1000, 10000, 1000, 1000, 10000}, 100, 100000, 1000, 10000})
                   .value();
    return {std::move(records), std::move(weighted), std::move(rec)};
}
const SnapshotWriteLimits limits{
    {{10000, 100000}, 100, 1000000, 100000}, {10000, 1000000, 10000000}};
SnapshotMetadata metadata(std::uint64_t version = 42) {
    return {base::SnapshotVersion::from_integer(version).value(),
        "original-build-1",
        {sha256_bytes("search-input").value(), sha256_bytes("search-config").value()},
        {sha256_bytes("recommendation-input").value(),
            sha256_bytes("recommendation-config").value()}};
}
base::Result<PublishedSnapshot> invoke(const fs::path &parent,
    const Fixture &input,
    const SnapshotWriteLimits &budget = limits,
    Fault fault = Fault::none,
    std::uint64_t version = 42) {
    state = {};
    state.active = true;
    state.fault = fault;
    struct Stop {
        ~Stop() {
            state.active = false;
        }
    } stop;
    return write_snapshot(parent.string(),
        input.records,
        input.search,
        input.recommendation,
        metadata(version),
        budget);
}
// 链接wrap只作用于测试进程的生产对象引用；辅助判定用real函数避免递归。
bool temporary_fd(int fd) {
    std::array<char, 4096> path{};
    const auto name = "/proc/self/fd/" + std::to_string(fd);
    const auto size = ::readlink(name.c_str(), path.data(), path.size() - 1);
    return size > 0 && std::strstr(path.data(), "/.siftwing-") != nullptr;
}
bool once(Fault fault) {
    if (state.active && state.fault == fault && state.hits == 0) {
        ++state.hits;
        return true;
    }
    return false;
}
} // namespace
extern "C" {
int __real_openat(int, const char *, int, ...);
ssize_t __real_write(int, const void *, size_t);
ssize_t __real_pread(int, void *, size_t, off_t);
ssize_t __real_getrandom(void *, size_t, unsigned);
int __real_mkdirat(int, const char *, mode_t);
int __real_fsync(int);
int __real_close(int);
int __real_fstat(int, struct stat *);
int __real_renameat2(int, const char *, int, const char *, unsigned);
DIR *__real_fdopendir(int);

int __wrap_openat(int directory, const char *path, int flags, ...) {
    // 生产Writer始终显式传mode；无O_CREAT时不需要读取变长参数。
    if (state.active) {
        ++state.open_calls;
        if (state.fault == Fault::interruptions && state.open_calls == 1) {
            errno = EINTR;
            return -1;
        }
        const bool output = (flags & O_CREAT) != 0, input = !(flags & O_DIRECTORY) && !output;
        if ((output && once(Fault::create_open)) || (input && once(Fault::reopen))
            || ((flags & O_DIRECTORY) && std::strncmp(path, ".siftwing-", 10) == 0
                && once(Fault::temp_open))) {
            errno = EACCES;
            return -1;
        }
        if (output && once(Fault::allocation)) {
            throw std::bad_alloc{};
        }
        const int fd = __real_openat(directory, path, flags, output ? 0600 : 0);
        if (fd >= 0) {
            if (output) {
                ++state.write_opens;
            }
            if (input) {
                ++state.read_opens;
            }
            if (std::strcmp(path, "COMPLETE") == 0) {
                state.complete_directory = directory;
            }
            if (input && std::strcmp(path, "COMPLETE") == 0 && once(Fault::extra)) {
                const int extra =
                    __real_openat(directory, "extra", O_CREAT | O_EXCL | O_WRONLY, 0600);
                if (extra >= 0) {
                    (void)__real_close(extra);
                }
            }
        }
        return fd;
    }
    return __real_openat(directory, path, flags, (flags & O_CREAT) ? 0600 : 0);
}
ssize_t __wrap_write(int fd, const void *data, size_t size) {
    if (state.active) {
        ++state.write_calls;
        if (state.fault == Fault::interruptions && state.write_calls == 1) {
            errno = EINTR;
            return -1;
        }
        if (once(Fault::zero_write)) {
            return 0;
        }
        if (once(Fault::write_error)) {
            errno = ENOSPC;
            return -1;
        }
        if (state.fault == Fault::short_io || state.fault == Fault::interruptions) {
            size = std::min(size, size_t{5});
        }
    }
    return __real_write(fd, data, size);
}
ssize_t __wrap_pread(int fd, void *data, size_t size, off_t offset) {
    if (state.active) {
        ++state.read_calls;
        if (state.fault == Fault::interruptions && state.read_calls == 1) {
            errno = EINTR;
            return -1;
        }
        if (once(Fault::eof)) {
            return 0;
        }
        if (once(Fault::read_error)) {
            errno = EIO;
            return -1;
        }
        if (state.fault == Fault::short_io || state.fault == Fault::interruptions) {
            size = std::min(size, size_t{3});
        }
    }
    const auto amount = __real_pread(fd, data, size, offset);
    if (amount > 0 && once(Fault::corruption)) {
        static_cast<char *>(data)[0] ^= 1;
    }
    return amount;
}
ssize_t __wrap_getrandom(void *data, size_t size, unsigned flags) {
    if (state.active) {
        ++state.random_calls;
        if (state.fault == Fault::interruptions && state.random_calls == 1) {
            errno = EINTR;
            return -1;
        }
        if (once(Fault::random_error)) {
            errno = EIO;
            return -1;
        }
        if (once(Fault::random_zero)) {
            return 0;
        }
        if (state.fault == Fault::collisions) {
            std::memset(data, 0, size);
            return static_cast<ssize_t>(size);
        }
        if (state.fault == Fault::interruptions) {
            size = std::min(size, size_t{3});
        }
    }
    return __real_getrandom(data, size, flags);
}
int __wrap_mkdirat(int fd, const char *name, mode_t mode) {
    if (once(Fault::mkdir_error)) {
        errno = EACCES;
        return -1;
    }
    return __real_mkdirat(fd, name, mode);
}
int __wrap_fstat(int fd, struct stat *info) {
    if (once(Fault::stat)) {
        errno = EIO;
        return -1;
    }
    return __real_fstat(fd, info);
}
int __wrap_fsync(int fd) {
    if (state.active) {
        ++state.sync_calls;
        if (state.fault == Fault::interruptions && state.sync_calls == 1) {
            errno = EINTR;
            return -1;
        }
        struct stat info {};
        const bool directory = __real_fstat(fd, &info) == 0 && S_ISDIR(info.st_mode);
        if ((!directory && once(Fault::file_sync))
            || (directory && !state.renamed && once(Fault::directory_sync))
            || (directory && state.renamed && once(Fault::parent_sync))) {
            errno = EIO;
            return -1;
        }
        if (directory) {
            ++state.directory_syncs;
        } else {
            ++state.file_syncs;
        }
    }
    return __real_fsync(fd);
}
int __wrap_close(int fd) {
    bool injected = false;
    if (state.active) {
        struct stat info {};
        const bool regular = __real_fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
        const bool temporary = temporary_fd(fd);
        if (regular && temporary) {
            const bool output = (::fcntl(fd, F_GETFL) & O_ACCMODE) == O_WRONLY;
            if (output) {
                ++state.write_closes;
            } else {
                ++state.read_closes;
            }
            injected = output ? once(Fault::write_close) : once(Fault::read_close);
            if (!output && state.complete_directory >= 0 && once(Fault::missing)) {
                (void)::unlinkat(state.complete_directory, "COMPLETE", 0);
            }
        } else if (!regular && temporary) {
            injected = once(Fault::directory_close);
        } else if (!regular && state.renamed) {
            injected = once(Fault::parent_close);
        }
    }
    const int code = __real_close(fd);
    if (injected) {
        errno = EIO;
        return -1;
    } // 实际先关闭，再模拟Linux close报错；必须只尝试一次。
    return code;
}
DIR *__wrap_fdopendir(int fd) {
    if (once(Fault::directory_stream)) {
        errno = ENOMEM;
        return nullptr;
    }
    return __real_fdopendir(fd);
}
int __wrap_renameat2(
    int olddir, const char *oldname, int newdir, const char *newname, unsigned flags) {
    if (state.active) {
        ++state.renames;
        state.ready_at_rename = state.write_opens == 8 && state.read_opens == 8
                                && state.file_syncs == 8 && state.write_closes == 8
                                && state.read_closes == 8 && state.directory_syncs == 1
                                && flags == RENAME_NOREPLACE;
        if (once(Fault::rename)) {
            errno = EACCES;
            return -1;
        }
        if (once(Fault::unsupported)) {
            errno = ENOSYS;
            return -1;
        }
        if (once(Fault::target_race)) {
            // 在Writer存在性检查之后插入外部空目录；普通rename会覆盖它，NOREPLACE必须拒绝。
            if (__real_mkdirat(newdir, newname, 0700) < 0) {
                errno = EIO;
                return -1;
            }
            struct stat info {};
            if (::fstatat(newdir, newname, &info, AT_SYMLINK_NOFOLLOW) < 0) {
                errno = EIO;
                return -1;
            }
            state.raced_inode = info.st_ino;
        }
    }
    const int code = __real_renameat2(olddir, oldname, newdir, newname, flags);
    if (code == 0 && state.active) {
        state.renamed = true;
    }
    return code;
}
} // extern C

int main(int argc, char **argv) {
    try {
        if (argc != 2 || ::getuid() == 0) {
            throw std::runtime_error("requires external build parent and ordinary user");
        }
        std::string pattern = (fs::path{argv[1]} / "writer-unit-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        const fs::path root{pattern};
        const auto baseline_fds = fd_count();
        unsigned sequence = 0;
        const auto parent = [&]() {
            auto p = root / std::to_string(++sequence);
            fs::create_directory(p);
            return p;
        };
        auto input = fixture();
        const auto first_parent = parent();
        const auto first = invoke(first_parent, input);
        check(first && first.value().durability == PublicationDurability::synced
                  && first.value().system_errno == 0,
            "published and synced");
        check(state.ready_at_rename && state.renames == 1 && state.directory_syncs == 2,
            "all files reopened/synced/closed before commit, then parent sync");
        check(entries(first_parent).size() == 1 && entries(first_parent / "42").size() == 8,
            "only complete formal directory remains");
        struct stat mode {};
        (void)::stat((first_parent / "42").c_str(), &mode);
        check((mode.st_mode & 0777) == 0700, "private directory mode");
        for (const auto &file : entries(first_parent / "42")) {
            (void)::stat(file.c_str(), &mode);
            check(S_ISREG(mode.st_mode) && (mode.st_mode & 0777) == 0600,
                "regular private payload mode");
        }
        const auto manifest_bytes = load(first_parent / "42/manifest.txt");
        check(static_cast<bool>(verify_completion(
                  manifest_bytes, load(first_parent / "42/COMPLETE"), limits.manifest)),
            "marker bound to saved manifest");
        check(first.value().manifest.search.documents == 3
                  && first.value().manifest.recommendation.documents == 5,
            "separate search/recommendation populations");
        const auto original_records = input.records;
        std::reverse(input.records.begin(), input.records.end());
        const auto second_parent = parent();
        const auto second = invoke(second_parent, input);
        check(static_cast<bool>(second), "record input permutation accepted");
        for (const auto &file : entries(first_parent / "42")) {
            check(load(file) == load(second_parent / "42" / file.filename()),
                "all eight files deterministic under record permutation");
        }
        auto repeated = invoke(first_parent, input);
        check(!repeated && repeated.error().context == "writer.exists"
                  && entries(first_parent).size() == 1,
            "repeated version preserves old directory");
        check(load(first_parent / "42/manifest.txt") == manifest_bytes, "old bytes unchanged");
        const auto raced_parent = parent();
        const auto raced = invoke(raced_parent, input, limits, Fault::target_race);
        (void)::stat((raced_parent / "42").c_str(), &mode);
        check(!raced && raced.error().context == "writer.rename" && state.ready_at_rename
                  && state.hits == 1,
            "target created after preflight rejected atomically");
        check(mode.st_ino == state.raced_inode && entries(raced_parent / "42").empty()
                  && entries(raced_parent).size() == 2,
            "external empty directory inode preserved plus own complete staging");
        for (auto version : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
            const auto p = parent();
            const auto result = invoke(p, input, limits, Fault::none, version);
            check(result && fs::is_directory(p / std::to_string(version)),
                "full opaque version range");
        }
        const auto empty_input = fixture(true);
        const auto empty_parent = parent();
        const auto empty = invoke(empty_parent, empty_input);
        check(empty && load(empty_parent / "42/documents.bin").empty()
                  && load(empty_parent / "42/offsets.bin").size() == 32,
            "complete empty search payloads");
        check(load(empty_parent / "42/search-lexicon.txt") == "SIFTWING-LEXICON 1\nS 0\n",
            "empty lexicon canonical");
        check(load(empty_parent / "42/recommendation-dictionary.txt")
                  == "SIFTWING-RECOMMENDATION 1\nS 0 0 0\n",
            "empty recommendation canonical");
        check(load(empty_parent / "42/recommendation-characters.txt")
                  == "SIFTWING-CHARACTERS 1\nS 0 0\n",
            "empty characters canonical");
        // 八个预算的零配置必须先拒绝，不创建任何临时对象。
        for (unsigned i = 0; i < 8; ++i) {
            auto budget = limits;
            std::array<std::uint64_t *, 8> values{{&budget.documents.record.max_field_bytes,
                &budget.documents.record.max_record_bytes,
                &budget.documents.max_documents,
                &budget.documents.max_library_bytes,
                &budget.documents.max_table_bytes,
                &budget.manifest.max_manifest_bytes,
                &budget.manifest.max_file_bytes,
                &budget.manifest.max_total_file_bytes}};
            *values[i] = 0;
            const auto p = parent();
            auto rejected = invoke(p, input, budget);
            check(!rejected && rejected.error().context == "writer.limits" && entries(p).empty(),
                "positive configuration required before IO");
        }
        // 独立从已落盘字节导出恰好边界，验证等于上限成功、上限减一失败。
        auto exact = limits;
        exact.documents.max_documents = 3;
        exact.documents.max_library_bytes = load(first_parent / "42/documents.bin").size();
        exact.documents.max_table_bytes = load(first_parent / "42/offsets.bin").size();
        exact.manifest.max_manifest_bytes = manifest_bytes.size();
        exact.manifest.max_file_bytes = 0;
        exact.manifest.max_total_file_bytes = 0;
        for (const auto &file : first.value().manifest.files) {
            exact.manifest.max_file_bytes = std::max(exact.manifest.max_file_bytes, file.bytes);
            exact.manifest.max_total_file_bytes += file.bytes;
        }
        check(static_cast<bool>(invoke(parent(), input, exact)), "all byte boundaries inclusive");
        for (unsigned i = 0; i < 6; ++i) {
            auto budget = exact;
            std::array<std::uint64_t *, 6> values{{&budget.documents.max_documents,
                &budget.documents.max_library_bytes,
                &budget.documents.max_table_bytes,
                &budget.manifest.max_manifest_bytes,
                &budget.manifest.max_file_bytes,
                &budget.manifest.max_total_file_bytes}};
            --*values[i];
            const auto p = parent();
            const auto rejected = invoke(p, input, budget);
            check(
                !rejected && entries(p).empty(), "budget below actual boundary rejected before IO");
        }
        for (unsigned bad = 0; bad < 3; ++bad) {
            auto records = input.records;
            if (bad == 0) {
                records.pop_back();
            } else if (bad == 1) {
                records[0].doc_id = records[1].doc_id;
            } else {
                records[0].doc_id = base::DocumentId::from_integer(1234).value();
            }
            const auto p = parent();
            const auto r = write_snapshot(
                p.string(), records, input.search, input.recommendation, metadata(), limits);
            check(!r && entries(p).empty(),
                "missing duplicate and foreign document identities refused");
        }
        const auto invalid_parent = parent();
        for (const auto &path :
            {std::string{}, std::string{"bad\0path", 8}, invalid_parent.string() + "/../other"}) {
            const auto r = write_snapshot(
                path, input.records, input.search, input.recommendation, metadata(), limits);
            check(!r && r.error().context == "writer.path", "path lexical rejection");
        }
        auto bad_metadata = metadata();
        bad_metadata.search_provenance.input_sha256 = "bad";
        auto r = write_snapshot(invalid_parent.string(),
            input.records,
            input.search,
            input.recommendation,
            bad_metadata,
            limits);
        check(!r && entries(invalid_parent).empty(), "invalid provenance refused before IO");
        bad_metadata = metadata();
        bad_metadata.build_version = "bad version";
        auto build_rejected = write_snapshot(invalid_parent.string(),
            input.records,
            input.search,
            input.recommendation,
            bad_metadata,
            limits);
        check(!build_rejected && entries(invalid_parent).empty(),
            "invalid build identity refused before IO");
        const auto link_parent = parent();
        fs::create_directory_symlink(invalid_parent, link_parent / "link");
        check(!invoke(link_parent / "link", input) && entries(invalid_parent).empty(),
            "parent symlink rejected");
        fs::create_directory(invalid_parent / "nested");
        check(!invoke(link_parent / "link/nested", input), "intermediate symlink rejected");
        check(!invoke(root / "absent", input) && !fs::exists(root / "absent"),
            "missing parent never created");
        const auto denied = parent();
        (void)::chmod(denied.c_str(), 0);
        check(!invoke(denied, input), "real ordinary-user permission denial");
        (void)::chmod(denied.c_str(), 0700);
        for (unsigned kind = 0; kind < 4; ++kind) {
            const auto p = parent();
            if (kind == 0) {
                std::ofstream{p / "42"} << "preserve";
            } else if (kind == 1) {
                fs::create_directory(p / "42");
            } else if (kind == 2) {
                fs::create_symlink("missing", p / "42");
            } else {
                if (::mkfifo((p / "42").c_str(), 0600) < 0) {
                    throw std::runtime_error("fifo fixture");
                }
            }
            const auto failed = invoke(p, input);
            check(!failed && failed.error().context == "writer.exists" && entries(p).size() == 1,
                "all existing target kinds preserved without blocking");
            if (kind == 0) {
                check(load(p / "42") == "preserve", "old file content preserved");
            }
        }
        for (const auto fault : {Fault::interruptions, Fault::short_io}) {
            const auto p = parent();
            const auto ok = invoke(p, input, limits, fault);
            check(ok && state.ready_at_rename,
                "EINTR and short IO recover without premature publication");
            for (const auto &file : entries(first_parent / "42")) {
                check(load(file) == load(p / "42" / file.filename()),
                    "short/interrupted IO bytes identical");
            }
        }
        for (const auto fault : {Fault::zero_write,
                 Fault::write_error,
                 Fault::eof,
                 Fault::read_error,
                 Fault::corruption,
                 Fault::file_sync,
                 Fault::directory_sync,
                 Fault::write_close,
                 Fault::read_close,
                 Fault::directory_close,
                 Fault::create_open,
                 Fault::reopen,
                 Fault::temp_open,
                 Fault::stat,
                 Fault::directory_stream,
                 Fault::extra,
                 Fault::missing,
                 Fault::rename,
                 Fault::unsupported}) {
            const auto p = parent();
            const auto before = fd_count();
            const auto failed = invoke(p, input, limits, fault);
            check(!failed && state.hits == 1 && !fs::exists(p / "42"),
                "precommit IO/verification fault never publishes");
            const auto residual = entries(p);
            check(residual.size() == 1
                      && failed.error().message.find(residual[0].filename().string())
                             != std::string::npos,
                "failed temporary directory explicitly preserved and diagnosed");
            check(fd_count() == before, "failure closes every owned descriptor without retry");
            if (fault == Fault::rename || fault == Fault::unsupported) {
                check(state.ready_at_rename && entries(residual[0]).size() == 8,
                    "rename failure preserves complete unpublished staging");
            }
        }
        for (const auto fault : {Fault::random_error, Fault::random_zero, Fault::mkdir_error}) {
            const auto p = parent();
            const auto before = fd_count();
            const auto failed = invoke(p, input, limits, fault);
            check(!failed && state.hits == 1 && entries(p).empty() && fd_count() == before,
                "creation failure leaves no falsely owned directory");
        }
        const auto collision_parent = parent();
        const auto collision =
            collision_parent / ".siftwing-42-00000000000000000000000000000000.tmp";
        fs::create_directory(collision);
        std::ofstream{collision / "sentinel"} << "preserve";
        const auto collision_result = invoke(collision_parent, input, limits, Fault::collisions);
        check(!collision_result && state.random_calls == 8
                  && load(collision / "sentinel") == "preserve",
            "bounded temp collisions preserve previously existing objects");
        for (const auto fault : {Fault::parent_sync, Fault::parent_close}) {
            const auto p = parent();
            const auto before = fd_count();
            const auto published = invoke(p, input, limits, fault);
            check(published && published.value().durability == PublicationDurability::unconfirmed
                      && published.value().system_errno == EIO,
                "postcommit error reports published but durability unconfirmed");
            check(state.ready_at_rename && entries(p).size() == 1 && fs::is_directory(p / "42")
                      && fd_count() == before,
                "postcommit error never rolls back or leaks fd");
            check(
                load(p / "42/manifest.txt") == manifest_bytes, "postcommit published bytes intact");
            check(!invoke(p, input), "retry cannot overwrite unconfirmed published version");
        }
        const auto exception_parent = parent();
        const auto before_exception = fd_count();
        bool threw = false;
        try {
            (void)invoke(exception_parent, input, limits, Fault::allocation);
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        check(threw && !fs::exists(exception_parent / "42") && entries(exception_parent).size() == 1
                  && fd_count() == before_exception,
            "escaping allocation exception releases fd and preserves only own staging");
        check(fd_count() == baseline_fds, "complete batch has no descriptor leak");
        for (const auto &record : input.records) {
            const auto saved = std::find_if(
                original_records.begin(), original_records.end(), [&](const auto &value) {
                    return value.doc_id == record.doc_id;
                });
            check(saved != original_records.end()
                      && document::encode_record(*saved, limits.documents.record).value()
                             == document::encode_record(record, limits.documents.record).value(),
                "every borrowed record field unchanged");
        }
        check(input.records.size() == original_records.size()
                  && input.search.raw_index().statistics().documents == 3
                  && input.recommendation.documents() == 5,
            "borrowed inputs remain usable");
        // 仅删除本测试mkdtemp新建树；从不清理调用方已有目录或任何项目私有输入。
        fs::remove_all(root);
        std::cout << "checks=" << checks << " failures=" << failures << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
