/*
 * PROJECT : SIFTWING
 * FILE    : writer_race_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 两个真实子进程完成同版本临时集合后同时竞争无覆盖rename，核单胜者及失败残留
 */
#include "siftwing/snapshot/writer.hpp"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int ready_fd = -1, release_fd = -1;
bool barrier_ok = false;
bool byte_io(int fd, char &byte, bool writing) {
    ssize_t count;
    do {
        count = writing ? ::write(fd, &byte, 1) : ::read(fd, &byte, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}
int child(const std::string &parent) {
    using namespace siftwing;
    try {
        const index::LexiconLimits ll{10, 100, 100, 10, 100};
        auto raw = index::InvertedIndex::build({}, {ll, 10, 10, 10, 10}).value();
        const auto weighted = index::WeightedIndex::build(std::move(raw), {10, 10, 10}).value();
        const auto rec = index::RecommendationDictionary::build({}, {ll, 10, 10, 10, 10}).value();
        const snapshot::SnapshotMetadata metadata{base::SnapshotVersion::from_integer(42).value(),
            "race-original-1",
            {std::string(64, 'a'), std::string(64, 'b')},
            {std::string(64, 'c'), std::string(64, 'd')}};
        const snapshot::SnapshotWriteLimits limits{
            {{100, 100}, 10, 1000, 1000}, {10000, 10000, 100000}};
        const auto result = snapshot::write_snapshot(parent, {}, weighted, rec, metadata, limits);
        if (!barrier_ok) {
            return 3;
        }
        if (result) {
            return result.value().durability == snapshot::PublicationDurability::synced ? 0 : 4;
        }
        return result.error().context == "writer.rename" ? 1 : 5;
    } catch (...) {
        return 6;
    }
}
} // namespace
extern "C" int __real_renameat2(int, const char *, int, const char *, unsigned);
extern "C" int __wrap_renameat2(
    int olddir, const char *oldname, int newdir, const char *newname, unsigned flags) {
    char byte = 'R';
    if (!byte_io(ready_fd, byte, true) || !byte_io(release_fd, byte, false)) {
        errno = EIO;
        return -1;
    }
    barrier_ok = byte == 'G';
    if (!barrier_ok) {
        errno = EIO;
        return -1;
    }
    return __real_renameat2(olddir, oldname, newdir, newname, flags);
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            return 2;
        }
        namespace fs = std::filesystem;
        std::string pattern = (fs::path{argv[1]} / "writer-race-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) {
            return 2;
        }
        int ready[2], release[2];
        if (::pipe(ready) < 0 || ::pipe(release) < 0) {
            return 2;
        }
        pid_t processes[2]{-1, -1};
        for (auto &process : processes) {
            process = ::fork();
            if (process == 0) {
                (void)::close(ready[0]);
                (void)::close(release[1]);
                ready_fd = ready[1];
                release_fd = release[0];
                const int code = child(pattern);
                (void)::close(ready_fd);
                (void)::close(release_fd);
                ::_exit(code);
            }
            if (process < 0) {
                // 创建失败时释放已创建子进程的屏障并回收，避免把失败测试留成孤立等待者。
                char byte = 'F';
                for (auto existing : processes) {
                    if (existing > 0) {
                        (void)byte_io(release[1], byte, true);
                    }
                }
                (void)::close(ready[0]);
                (void)::close(ready[1]);
                (void)::close(release[0]);
                (void)::close(release[1]);
                for (auto existing : processes) {
                    if (existing > 0) {
                        while (::waitpid(existing, nullptr, 0) < 0 && errno == EINTR) {
                        }
                    }
                }
                throw std::runtime_error("fork failed");
            }
        }
        (void)::close(ready[1]);
        (void)::close(release[0]);
        bool all_ready = true;
        for (unsigned i = 0; i < 2; ++i) {
            struct pollfd descriptor {
                ready[0], POLLIN, 0
            };
            int code;
            do {
                code = ::poll(&descriptor, 1, 10000);
            } while (code < 0 && errno == EINTR);
            char byte = 0;
            if (code <= 0 || !byte_io(ready[0], byte, false) || byte != 'R') {
                all_ready = false;
                break;
            }
        }
        for (unsigned i = 0; i < 2; ++i) {
            char byte = all_ready ? 'G' : 'F';
            (void)byte_io(release[1], byte, true);
        }
        (void)::close(ready[0]);
        (void)::close(release[1]);
        unsigned winners = 0, losers = 0, failures = 0;
        for (auto process : processes) {
            int status = 0;
            pid_t waited;
            do {
                waited = ::waitpid(process, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited != process || !WIFEXITED(status)) {
                ++failures;
            } else if (WEXITSTATUS(status) == 0) {
                ++winners;
            } else if (WEXITSTATUS(status) == 1) {
                ++losers;
            } else {
                ++failures;
            }
        }
        unsigned checks = 0;
        const auto check = [&](bool ok) {
            ++checks;
            if (!ok) {
                ++failures;
            }
        };
        check(all_ready && winners == 1 && losers == 1);
        const fs::path root{pattern};
        unsigned formal = 0, staging = 0;
        for (const auto &entry : fs::directory_iterator(root)) {
            if (entry.path().filename() == "42") {
                ++formal;
            } else {
                ++staging;
                check(entry.path().filename().string().find(".siftwing-42-") == 0);
            }
            unsigned files = 0;
            for (const auto &file : fs::directory_iterator(entry.path())) {
                ++files;
                check(file.is_regular_file() && !file.is_symlink());
            }
            check(files == 8);
        }
        check(formal == 1 && staging == 1);
        check(fs::is_regular_file(root / "42/manifest.txt")
              && fs::is_regular_file(root / "42/COMPLETE"));
        const auto read = [](const fs::path &path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                throw std::runtime_error("race output missing");
            }
            return std::string{
                std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        };
        for (const auto &entry : fs::directory_iterator(root)) {
            if (entry.path().filename() == "42") {
                continue;
            }
            for (const auto &file : fs::directory_iterator(root / "42")) {
                check(read(file.path()) == read(entry.path() / file.path().filename()));
            }
        }
        fs::remove_all(root); // 仅本测试全新mkdtemp树。
        std::cout << "checks=" << checks << " failures=" << failures
                  << "; real two-process NOREPLACE race\n";
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
