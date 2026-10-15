/*
 * PROJECT : SIFTWING
 * FILE    : server_settings_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 独立期望验证配置覆盖、拥有关系、严格JSON与文件准入
 */
#include "siftwing/config/server_settings.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

namespace {
enum class ReadFault {
    none,
    short_read,
    interrupted,
    failed,
    changed
};
ReadFault read_fault = ReadFault::none;
std::string changing_path;
std::size_t read_attempts = 0;
using siftwing::config::load_server_settings;
std::size_t checks = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok)
        throw std::runtime_error(label);
}
void write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    file.close();
    check(bool(file), "写原创夹具");
}
void bad(const std::vector<std::string> &args, const char *context = nullptr) {
    const auto result = load_server_settings(args);
    check(!result, "拒绝非法配置");
    if (context)
        check(result.error().context == context, "错误阶段");
}
std::size_t fds() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
}
// 只在同步配置装载期间启用；真实fd提供字节，注入可确定复现的短读/系统失败/文件变化。
extern "C" ssize_t __real_read(int, void *, std::size_t);
extern "C" ssize_t __wrap_read(int fd, void *buffer, std::size_t count) {
    ++read_attempts;
    if (read_fault == ReadFault::interrupted || read_fault == ReadFault::failed) {
        errno = read_fault == ReadFault::interrupted ? EINTR : EIO;
        return -1;
    }
    const auto result = __real_read(fd, buffer, read_fault == ReadFault::short_read ? 3 : count);
    if (read_fault == ReadFault::changed && result > 0) {
        read_fault = ReadFault::none;
        if (::truncate(changing_path.c_str(), 0) != 0)
            std::terminate();
    }
    return result;
}
int main() {
    char pattern[] = "/tmp/siftwing-settings-XXXXXX";
    const char *made = ::mkdtemp(pattern);
    if (!made)
        return 1;
    const std::filesystem::path root(made);
    try {
        auto defaults = load_server_settings({"--snapshot", "relative", "--version", "0"});
        check(bool(defaults), "旧CLI含零版本");
        const auto &d = defaults.value();
        check(d.service.parent == "relative" && d.server.sub_count == 2 && d.server.workers == 4
                  && d.service.cache.capacity == 0 && d.server.sub.idle_timeout_ms == 0
                  && d.shutdown_ms == 1000 && d.listen_address == "127.0.0.1",
            "兼容默认值");
        check(d.log_level == siftwing::logging::Level::info && d.log_file.empty(),
            "默认info到stderr");
        check(d.log_rotation.max_bytes == 10485760 && d.log_rotation.max_files == 3,
            "默认10MiB和三份归档");
        for (const auto &level : {"off", "debug", "info", "warn", "error"}) {
            auto value =
                load_server_settings({"--snapshot", "p", "--version", "0", "--log-level", level});
            check(bool(value), "五种固定日志级别");
        }
        bad({"--snapshot", "p", "--version", "0", "--log-level", "verbose"}, "config.value");
        auto file_default =
            load_server_settings({"--snapshot", "p", "--version", "0", "--log-file", "new"});
        check(bool(file_default) && file_default.value().log_level == siftwing::logging::Level::info
                  && file_default.value().log_file == "new",
            "只指定文件仍默认启用info");
        bad({"--snapshot", "p", "--version", "0", "--log-level", "off", "--log-file", "new"},
            "config.value");
        for (const auto &pair : {std::pair{"--log-max-bytes", "2047"},
                 std::pair{"--log-max-bytes", "1073741825"},
                 std::pair{"--log-max-files", "0"},
                 std::pair{"--log-max-files", "21"}})
            bad({"--snapshot", "p", "--version", "0", pair.first, pair.second}, "config.value");
        const auto config = root / "config.json";
        write(config,
            R"({"schema_version":1,"snapshot":"cwd-relative","version":"18446744073709551615",
          "listen_address":"::1","subs":3,"workers":5,"port":12345,"task_capacity":9,
          "hmm":false,"tokenizer":"cppjieba","dictionary":"dict","model":"hmm","user_dictionary":"user",
          "stopwords":"stop","cache_capacity":17,"cache_bytes":1024,"cache_max_value_bytes":1000,
          "idle_timeout_ms":19,"request_timeout_ms":29,"run_ms":59,"shutdown_ms":79,
          "handoff_capacity":7,"max_connections":11,"completion_capacity":13,
          "handoff_budget":3,"completion_budget":5,"io_attempt_budget":7,"accept_attempt_budget":9,
          "max_body_bytes":1024,"read_byte_budget":33,"write_byte_budget":55,
          "output_low_bytes":25,"output_high_bytes":49,"output_hard_bytes":100,
          "max_query_bytes":20,"max_top_k":2,"max_work":999,"max_output_bytes":123,
          "max_snapshot_bytes":5678,"max_resource_bytes":2345,"backlog":17,
          "tcp_no_delay":true,"reuse_address":true,"log_level":"debug","log_file":"new-log",
          "log_max_bytes":4096,"log_max_files":2})");
        auto loaded =
            load_server_settings({"--workers", "6", "--config", config.string(), "--port", "0"});
        check(bool(loaded), "文件全部字段与前后CLI覆盖");
        check(loaded.value().log_level == siftwing::logging::Level::debug
                  && loaded.value().log_file == "new-log"
                  && loaded.value().log_rotation.max_bytes == 4096
                  && loaded.value().log_rotation.max_files == 2,
            "文件日志映射");
        auto moved = std::move(loaded).value();
        auto copied = moved;
        moved.listen_address = "127.0.0.1";
        const auto c = siftwing::config::online_config(copied);
        check(c.listen.address == "::1" && c.listen.family == siftwing::net::IpFamily::v6
                  && c.listen.address.data() == copied.listen_address.data(),
            "移动复制后绑定拥有地址");
        check(copied.service.parent == "cwd-relative" && copied.service.version == UINT64_MAX
                  && c.workers == 6 && c.sub_count == 3 && c.listen.port == 0
                  && c.task_capacity == 9 && !copied.service.hmm && copied.service.user == "user"
                  && copied.service.stopwords == "stop",
            "字符串与整数覆盖");
        check(c.sub.handoff_capacity == 7 && c.sub.max_connections == 11
                  && c.sub.completion_capacity == 13 && c.sub.handoff_budget == 3
                  && c.sub.completion_budget == 5 && c.sub.io_attempt_budget == 7
                  && c.main.accept_attempt_budget == 9 && c.sub.max_body_bytes == 1024
                  && c.sub.read_byte_budget == 33 && c.sub.write_byte_budget == 55
                  && c.sub.output.low_bytes == 25 && c.sub.output.high_bytes == 49
                  && c.sub.output.hard_bytes == 100,
            "网络容量与预算映射");
        check(copied.service.cache.capacity == 17 && copied.service.cache.bytes == 1024
                  && copied.service.cache.max_value_bytes == 1000 && c.sub.idle_timeout_ms == 19
                  && c.sub.request_timeout_ms == 29 && copied.run_ms == 59
                  && copied.shutdown_ms == 79 && copied.service.query_bytes == 20
                  && copied.service.max_k == 2 && copied.service.work == 999
                  && copied.service.output_bytes == 123 && copied.service.snapshot_bytes == 5678
                  && copied.service.resource_bytes == 2345 && c.listen.backlog == 17
                  && c.listen.reuse_address && c.main.tcp_no_delay,
            "查询缓存期限映射");
        const std::vector<std::string> base{"--snapshot", "p", "--version", "1"};
        for (const auto &tail :
            std::vector<std::vector<std::string>>{{"--workers", "0"},
                {"--workers", "65"},
                {"--workers", "01"},
                {"--port", "-1"},
                {"--port", "+1"},
                {"--version", "18446744073709551616"},
                {"--run-ms", "86400001"},
                {"--hmm", "on"},
                {"--tokenizer", "cppjieba"},
                {"--dictionary", "d"},
                {"--listen-address", "localhost"},
                {"--listen-address", "::1%lo"},
                {"--tcp-no-delay", "true"},
                {"--cache-bytes", "1"},
                {"--output-low-bytes", "1"},
                {"--output-high-bytes", "100", "--output-hard-bytes", "99"},
                {"--max-body-bytes", "2", "--output-high-bytes", "25", "--output-hard-bytes", "27"},
                {"--no-such", "1"},
                {"--max_body_bytes", "10"},
                {"--port"},
                {"--workers", "2", "--workers", "3"}}) {
            auto args = base;
            args.insert(args.end(), tail.begin(), tail.end());
            bad(args);
        }
        bad({}, "config.required");
        bad({"--snapshot", "p"}, "config.required");
        bad({"--version", "0"}, "config.required");
        bad({"--config", config.string(), "--config", config.string()}, "config.argument");
        // 原始JSON独立写出，包括转义同名键；不由生产序列化器制造期望。
        for (const std::string text : {"",
                 "[]",
                 "null",
                 "true",
                 "{}",
                 "{",
                 "{\"schema_version\":1,}",
                 "{\"schema_version\":1.0}",
                 "{\"schema_version\":true}",
                 "{\"schema_version\":2}",
                 "{\"schema_version\":1,\"workers\":2,\"work\\u0065rs\":3}",
                 "{\"schema_version\":1,\"workers\":{}}",
                 "{\"schema_version\":1,\"workers\":[]}",
                 "{\"schema_version\":1,\"workers\":\"2\"}",
                 "{\"schema_version\":1,\"workers\":2.0}",
                 "{\"schema_version\":1,\"workers\":-1}",
                 "{\"schema_version\":1,\"workers\":null}",
                 "{\"schema_version\":1,\"version\":1}",
                 "{\"schema_version\":1,\"version\":\"01\"}",
                 "{\"schema_version\":1,\"hmm\":\"off\"}",
                 "{\"schema_version\":1,\"unknown\":1}",
                 "{\"schema_version\":1,\"listen_address\":\"bad\"}",
                 "{\"schema_version\":1,\"snapshot\":\"a\\u0000b\"}"}) {
            write(config, text);
            auto args = base;
            args.insert(args.end(), {"--config", config.string(), "--workers", "2"});
            bad(args); // CLI不能掩盖非法文件值。
        }
        write(config, std::string("{\"schema_version\":1,\"snapshot\":\"") + char(0xff) + "\"}");
        bad({"--config", config.string()}, "config.json");
        write(config, std::string(65537, ' '));
        bad({"--config", config.string()}, "config.file");
        write(config, "{\"schema_version\":1}" + std::string(65516, ' '));
        check(std::filesystem::file_size(config) == 65536, "文件精确边界");
        check(bool(load_server_settings(
                  {"--config", config.string(), "--snapshot", "p", "--version", "0"})),
            "64KiB允许");
        std::filesystem::create_symlink(config, root / "link");
        std::filesystem::create_directory_symlink(root, root / "dirlink");
        check(::mkfifo((root / "fifo").c_str(), 0600) == 0, "FIFO夹具");
        const auto fd_before = fds();
        write(config, "{\"schema_version\":1}");
        read_fault = ReadFault::short_read;
        check(bool(load_server_settings(
                  {"--config", config.string(), "--snapshot", "p", "--version", "0"})),
            "短读仍完整解析");
        for (const auto fault : {ReadFault::interrupted, ReadFault::failed, ReadFault::changed}) {
            write(config, "{\"schema_version\":1}");
            changing_path = config.string();
            read_attempts = 0;
            read_fault = fault;
            bad({"--config", config.string()}, "config.file");
            if (fault == ReadFault::interrupted)
                check(read_attempts == 129, "持续EINTR有界退出");
            read_fault = ReadFault::none;
        }
        for (int repeat = 0; repeat < 20; ++repeat)
            for (const auto &path : {root / "link",
                     root / "dirlink/config.json",
                     root / "fifo",
                     root / "missing",
                     root,
                     root / "../outside"})
                bad({"--config", path.string()}, "config.file");
        check(fds() == fd_before, "全部失败路径无fd泄漏");
        std::filesystem::remove_all(root);
        std::cout << "server settings checks=" << checks << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << "；失败原创夹具保留：" << root << '\n';
        return 1;
    }
}
