/*
 * PROJECT : SIFTWING
 * FILE    : server_settings.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 有界配置文件读取、严格字段准入与启动前合并
 */
#include "siftwing/config/server_settings.hpp"
#include "siftwing/net/unique_fd.hpp"
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <climits>
#include <set>
#include <stdexcept>
#include <utility>

namespace siftwing::config {
namespace {
using Json = nlohmann::json;
struct Rejected {
    std::string context, message;
};
[[noreturn]] void reject(const char *context, const std::string &message) {
    throw Rejected{context, message};
}
enum class Kind {
    number,
    text,
    boolean
};
// 字段表是文件类型与单字段范围的唯一入口；赋值仍显式对应底层已存在的字段。
struct Field {
    const char *key;
    Kind kind;
    std::uint64_t min = 0, max = 0;
};
constexpr Field fields[] = {{"snapshot", Kind::text},
    {"version", Kind::text},
    {"log_level", Kind::text},
    {"log_file", Kind::text},
    {"log_max_bytes", Kind::number, 2048, 1073741824},
    {"log_max_files", Kind::number, 1, 20},
    {"listen_address", Kind::text},
    {"tokenizer", Kind::text},
    {"stopwords", Kind::text},
    {"dictionary", Kind::text},
    {"model", Kind::text},
    {"user_dictionary", Kind::text},
    {"hmm", Kind::boolean},
    {"tcp_no_delay", Kind::boolean},
    {"reuse_address", Kind::boolean},
    {"port", Kind::number, 0, 65535},
    {"backlog", Kind::number, 1, INT_MAX},
    {"subs", Kind::number, 1, 64},
    {"workers", Kind::number, 1, 64},
    {"task_capacity", Kind::number, 1, 1024},
    {"handoff_capacity", Kind::number, 1, 1024},
    {"max_connections", Kind::number, 1, 1024},
    {"handoff_budget", Kind::number, 1, 1024},
    {"io_attempt_budget", Kind::number, 1, 1024},
    {"completion_capacity", Kind::number, 1, 1024},
    {"completion_budget", Kind::number, 1, 1024},
    {"accept_attempt_budget", Kind::number, 1, 1024},
    {"max_body_bytes", Kind::number, 2, 1048576},
    {"read_byte_budget", Kind::number, 1, 1048600},
    {"write_byte_budget", Kind::number, 1, 1048600},
    {"output_low_bytes", Kind::number, 0, 1048600},
    {"output_high_bytes", Kind::number, 0, 1048600},
    {"output_hard_bytes", Kind::number, 0, 1048600},
    {"idle_timeout_ms", Kind::number, 0, 86400000},
    {"request_timeout_ms", Kind::number, 0, 86400000},
    {"run_ms", Kind::number, 0, 86400000},
    {"shutdown_ms", Kind::number, 0, 86400000},
    {"max_query_bytes", Kind::number, 1, 65536},
    {"max_top_k", Kind::number, 1, 100},
    {"max_work", Kind::number, 1, 50000000},
    {"max_output_bytes", Kind::number, 1, 1048576},
    {"max_snapshot_bytes", Kind::number, 1, 536870912},
    {"max_resource_bytes", Kind::number, 1, 16777216},
    {"cache_capacity", Kind::number, 0, 4096},
    {"cache_bytes", Kind::number, 1, 67108864},
    {"cache_max_value_bytes", Kind::number, 1, 1048576}};
const Field &field(const std::string &key) {
    for (const auto &candidate : fields)
        if (key == candidate.key)
            return candidate;
    reject("config.argument", "未知配置字段：" + key);
}
std::uint64_t number(const std::string &text, std::uint64_t min, std::uint64_t max) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || std::to_string(value) != text || value < min || value > max)
        reject("config.value", "整数格式或范围非法");
    return value;
}
void address(const std::string &text) {
    unsigned char bytes[16]{};
    if (::inet_pton(text.find(':') == std::string::npos ? AF_INET : AF_INET6, text.c_str(), bytes)
        != 1)
        reject("config.value", "监听地址必须是数值IPv4或IPv6");
}
void apply(ServerSettings &s, const Field &f, const std::string &v) {
    const std::string key = f.key;
    if (v.empty() || v.find('\0') != std::string::npos)
        reject("config.value", "字段为空或含NUL：" + key);
    if (f.kind == Kind::text) {
        if (key == "log_file")
            s.log_file = v;
        else if (key == "log_level") {
            if (v == "off")
                s.log_level = logging::Level::off;
            else if (v == "debug")
                s.log_level = logging::Level::debug;
            else if (v == "info")
                s.log_level = logging::Level::info;
            else if (v == "warn")
                s.log_level = logging::Level::warn;
            else if (v == "error")
                s.log_level = logging::Level::error;
            else
                reject("config.value", "未知日志级别");
        } else if (key == "snapshot")
            s.service.parent = v;
        else if (key == "version")
            s.service.version = number(v, 0, UINT64_MAX);
        else if (key == "listen_address") {
            address(v);
            s.listen_address = v;
        } else if (key == "tokenizer") {
            if (v != "english" && v != "cppjieba")
                reject("config.value", "未知分词器");
            s.service.tokenizer = v;
        } else if (key == "stopwords")
            s.service.stopwords = v;
        else if (key == "dictionary")
            s.service.dictionary = v;
        else if (key == "model")
            s.service.model = v;
        else if (key == "user_dictionary")
            s.service.user = v;
        return;
    }
    if (f.kind == Kind::boolean) {
        if (v != "on" && v != "off")
            reject("config.value", "开关必须为on或off");
        const bool value = v == "on";
        if (key == "hmm")
            s.service.hmm = value;
        else if (key == "tcp_no_delay")
            s.server.main.tcp_no_delay = value;
        else if (key == "reuse_address")
            s.server.listen.reuse_address = value;
        return;
    }
    const auto n = number(v, f.min, f.max);
    if (key == "log_max_bytes")
        s.log_rotation.max_bytes = static_cast<std::size_t>(n);
    else if (key == "log_max_files")
        s.log_rotation.max_files = static_cast<std::size_t>(n);
    else if (key == "port")
        s.server.listen.port = static_cast<std::uint16_t>(n);
    else if (key == "backlog")
        s.server.listen.backlog = static_cast<int>(n);
    else if (key == "subs")
        s.server.sub_count = static_cast<std::size_t>(n);
    else if (key == "workers")
        s.server.workers = static_cast<std::size_t>(n);
    else if (key == "task_capacity")
        s.server.task_capacity = static_cast<std::size_t>(n);
    else if (key == "handoff_capacity")
        s.server.sub.handoff_capacity = static_cast<std::size_t>(n);
    else if (key == "max_connections")
        s.server.sub.max_connections = static_cast<std::size_t>(n);
    else if (key == "handoff_budget")
        s.server.sub.handoff_budget = static_cast<std::size_t>(n);
    else if (key == "io_attempt_budget")
        s.server.sub.io_attempt_budget = static_cast<std::size_t>(n);
    else if (key == "completion_capacity")
        s.server.sub.completion_capacity = static_cast<std::size_t>(n);
    else if (key == "completion_budget")
        s.server.sub.completion_budget = static_cast<std::size_t>(n);
    else if (key == "accept_attempt_budget")
        s.server.main.accept_attempt_budget = static_cast<std::size_t>(n);
    else if (key == "max_body_bytes")
        s.server.sub.max_body_bytes = static_cast<std::size_t>(n);
    else if (key == "read_byte_budget")
        s.server.sub.read_byte_budget = static_cast<std::size_t>(n);
    else if (key == "write_byte_budget")
        s.server.sub.write_byte_budget = static_cast<std::size_t>(n);
    else if (key == "output_low_bytes")
        s.server.sub.output.low_bytes = static_cast<std::size_t>(n);
    else if (key == "output_high_bytes")
        s.server.sub.output.high_bytes = static_cast<std::size_t>(n);
    else if (key == "output_hard_bytes")
        s.server.sub.output.hard_bytes = static_cast<std::size_t>(n);
    else if (key == "idle_timeout_ms")
        s.server.sub.idle_timeout_ms = static_cast<std::int64_t>(n);
    else if (key == "request_timeout_ms")
        s.server.sub.request_timeout_ms = static_cast<std::int64_t>(n);
    else if (key == "run_ms")
        s.run_ms = n;
    else if (key == "shutdown_ms")
        s.shutdown_ms = static_cast<std::int64_t>(n);
    else if (key == "max_query_bytes")
        s.service.query_bytes = n;
    else if (key == "max_top_k")
        s.service.max_k = n;
    else if (key == "max_work")
        s.service.work = n;
    else if (key == "max_output_bytes")
        s.service.output_bytes = n;
    else if (key == "max_snapshot_bytes")
        s.service.snapshot_bytes = n;
    else if (key == "max_resource_bytes")
        s.service.resource_bytes = n;
    else if (key == "cache_capacity")
        s.service.cache.capacity = static_cast<std::size_t>(n);
    else if (key == "cache_bytes")
        s.service.cache.bytes = static_cast<std::size_t>(n);
    else if (key == "cache_max_value_bytes")
        s.service.cache.max_value_bytes = static_cast<std::size_t>(n);
}
std::string read_file(const std::string &path) {
    if (path.empty() || path.find('\0') != std::string::npos || path.back() == '/')
        reject("config.file", "非法配置路径");
    net::UniqueFd dir(::open(path.front() == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0)
        reject("config.file", "打开路径根失败");
    std::size_t begin = path.front() == '/' ? 1 : 0;
    net::UniqueFd file;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const bool last = end == std::string::npos;
        const auto part = path.substr(begin, last ? path.size() - begin : end - begin);
        if (part == "..")
            reject("config.file", "配置路径禁止..");
        if (!part.empty() && part != ".") {
            net::UniqueFd next(::openat(dir.get(),
                part.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? O_NONBLOCK : O_DIRECTORY)));
            if (next.get() < 0)
                reject("config.file", "打开配置路径失败");
            if (last)
                file = std::move(next);
            else
                dir = std::move(next);
        }
        if (last)
            break;
        begin = end + 1;
    }
    struct stat before {
    }, after{};
    constexpr std::size_t cap = 65536;
    if (file.get() < 0 || ::fstat(file.get(), &before) < 0 || !S_ISREG(before.st_mode)
        || before.st_size <= 0 || before.st_size > static_cast<off_t>(cap))
        reject("config.file", "配置必须为非空且不超过64KiB的普通文件");
    std::string text;
    char buffer[4096];
    unsigned interruptions = 0;
    for (;;) {
        const auto remaining = cap + 1 - text.size();
        const auto count =
            ::read(file.get(), buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR && ++interruptions <= 128)
                continue;
            reject("config.file", "读取配置失败");
        }
        if (count == 0)
            break;
        text.append(buffer, static_cast<std::size_t>(count));
        if (text.size() > cap)
            reject("config.file", "读取期间配置超大");
    }
    // 拒绝观察到的原地修改；fd固定对象，不把路径再次stat当作同一对象证明。
    if (::fstat(file.get(), &after) < 0 || before.st_dev != after.st_dev
        || before.st_ino != after.st_ino || before.st_size != after.st_size
        || static_cast<off_t>(text.size()) != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec
        || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        reject("config.file", "读取期间配置发生变化");
    return text;
}
Json parse(const std::string &text) {
    std::set<std::string> keys;
    try {
        // 在parser递归前拒绝所有子容器；键回调取得解码后文本，转义不能绕过重复检查。
        auto result = Json::parse(text, [&keys](int depth, Json::parse_event_t event, Json &value) {
            if (event == Json::parse_event_t::array_start
                || (event == Json::parse_event_t::object_start && depth != 0))
                reject("config.json", "配置只允许平面对象");
            if (event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
                reject("config.json", "重复配置键");
            return true;
        });
        if (!result.is_object())
            reject("config.json", "配置根必须为对象");
        return result;
    } catch (const Json::exception &) {
        reject("config.json", "非法JSON或UTF-8");
    }
}
} // namespace

base::Result<ServerSettings> load_server_settings(const std::vector<std::string> &arguments) {
    try {
        ServerSettings settings;
        std::set<std::string> cli_seen, supplied;
        std::vector<std::pair<std::string, std::string>> overrides;
        std::string config;
        for (std::size_t i = 0; i < arguments.size(); i += 2) {
            const auto &flag = arguments[i];
            if (i + 1 == arguments.size() || flag.size() < 3 || flag.substr(0, 2) != "--"
                || !cli_seen.insert(flag).second || arguments[i + 1].empty())
                reject("config.argument", "选项重复、缺值或格式非法");
            if (flag == "--config") {
                config = arguments[i + 1];
                continue;
            }
            auto key = flag.substr(2);
            // CLI仅接受kebab-case，文件仅接受snake_case，不引入两组别名。
            if (key.find('_') != std::string::npos)
                reject("config.argument", "未知命令行选项");
            for (auto &c : key)
                if (c == '-')
                    c = '_';
            (void)field(key);
            overrides.emplace_back(std::move(key), arguments[i + 1]);
        }
        if (!config.empty()) {
            const auto object = parse(read_file(config));
            if (!object.contains("schema_version") || !object["schema_version"].is_number_integer()
                || object["schema_version"] != 1)
                reject("config.json", "schema_version必须为整数1");
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (it.key() == "schema_version")
                    continue;
                const auto &f = field(it.key());
                const auto &value = it.value();
                std::string text;
                if (f.kind == Kind::text && value.is_string())
                    text = value.get<std::string>();
                else if (f.kind == Kind::boolean && value.is_boolean())
                    text = value.get<bool>() ? "on" : "off";
                else if (f.kind == Kind::number && value.is_number_integer()
                         && (value.is_number_unsigned() || value.get<std::int64_t>() >= 0))
                    text = std::to_string(value.get<std::uint64_t>());
                else
                    reject("config.value", "配置字段类型错误：" + it.key());
                apply(settings, f, text);
                supplied.insert(it.key());
            }
        }
        for (const auto &item : overrides) {
            apply(settings, field(item.first), item.second);
            supplied.insert(item.first);
        }
        if (!supplied.count("snapshot") || !supplied.count("version"))
            reject("config.required", "必须显式指定snapshot和version");
        if (settings.log_level == logging::Level::off && !settings.log_file.empty())
            reject("config.value", "日志文件要求启用日志级别");
        const auto &s = settings.service;
        if ((s.tokenizer == "english"
                && (supplied.count("hmm") || !s.dictionary.empty() || !s.model.empty()
                    || !s.user.empty()))
            || (s.tokenizer == "cppjieba" && (s.dictionary.empty() || s.model.empty()))
            || s.cache.max_value_bytes > s.cache.bytes)
            reject("config.value", "资源或缓存预算不相容");
        try {
            (void)net::checked_output_watermarks(
                settings.server.sub.max_body_bytes, settings.server.sub.output);
        } catch (const std::invalid_argument &) {
            reject("config.value", "输出水位不相容");
        }
        return base::Result<ServerSettings>::success(std::move(settings));
    } catch (const Rejected &error) {
        return base::Result<ServerSettings>::failure({error.message, error.context});
    }
}
net::OnlineServerConfig online_config(const ServerSettings &settings) {
    auto config = settings.server;
    config.listen.address = settings.listen_address;
    config.listen.family = settings.listen_address.find(':') == std::string::npos
                               ? net::IpFamily::v4
                               : net::IpFamily::v6;
    return config;
}
} // namespace siftwing::config
