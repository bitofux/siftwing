/*
 * PROJECT : SIFTWING
 * FILE    : network.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 四命令网络模式的严格参数与完整输出
 * IMPLEMENTATION : 请求构造与网络交换分离；错误响应仅stderr，不把部分JSON交stdout。
 */
#include "network.hpp"
#include "siftwing/net/client.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <iostream>
#include <set>
#include <stdexcept>

namespace siftwing::cli {
const char *network_help() noexcept {
    return R"(
网络模式：siftwing-cli --network PING|SERVER_INFO|SEARCH|SUGGEST [选项]
  --host <数字IP>              IPv4或IPv6（默认127.0.0.1，无DNS）
  --port <N>                   服务端端口（必需，1..65535）
  --timeout-ms <N>             连接/发送/接收总期限（默认5000，1..86400000）
  --request-id <N>             关联号（默认1，非零uint64）
  --max-response-bytes <N>     响应Body上限（默认1048576，2..1048576）
  --query <文本>              SEARCH必需
  --mode and|or               SEARCH匹配方式（默认or）
  --top-k <N>                 SEARCH/SUGGEST返回条数（默认10，1..100）
  --summary-window <N>        SEARCH摘要窗口（默认160，1..4096）
  --summary-context <N>       SEARCH前文（默认40，0..window-1）
  --key <词键>                SUGGEST必需
  --normalize-key             SUGGEST规范化（默认关闭）
网络模式不接纳本地快照或分词资源选项；远端ERROR退出5，诊断在stderr。
)";
}
namespace {
[[noreturn]] void bad() {
    throw std::invalid_argument("非法网络模式参数；使用--help查看合同");
}
std::uint64_t number(const std::string &text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || std::to_string(value) != text)
        bad();
    return value;
}
}
int network_main(int argc, char **argv) {
    using namespace net;
    ClientConfig config;
    Frame request{MessageType::ping, 0, 1, "{}"};
    try {
        if (argc < 3)
            bad();
        const std::string command = argv[2];
        if (command == "SERVER_INFO")
            request.message_type = MessageType::server_info_request;
        else if (command == "SEARCH")
            request.message_type = MessageType::search_request;
        else if (command == "SUGGEST")
            request.message_type = MessageType::suggest_request;
        else if (command != "PING")
            bad();
        nlohmann::json body = nlohmann::json::object();
        std::set<std::string> seen;
        std::uint64_t window = 160, context = 40;
        for (int i = 3; i < argc; ++i) {
            const std::string flag = argv[i];
            if (!seen.insert(flag).second)
                bad();
            if (flag == "--normalize-key") {
                if (command != "SUGGEST")
                    bad();
                body["normalize_key"] = true;
                continue;
            }
            if (++i == argc)
                bad();
            const std::string value = argv[i];
            if (flag == "--host")
                config.host = value;
            else if (flag == "--port") {
                const auto port = number(value);
                if (!port || port > 65535)
                    bad();
                config.port = static_cast<std::uint16_t>(port);
            } else if (flag == "--timeout-ms")
                config.timeout_ms = number(value);
            else if (flag == "--max-response-bytes") {
                const auto cap = number(value);
                if (cap < 2 || cap > frame_max_body_bytes)
                    bad();
                config.max_response_bytes = static_cast<std::size_t>(cap);
            } else if (flag == "--request-id") {
                request.request_id = number(value);
                if (!request.request_id)
                    bad();
            } else if (flag == "--query" && command == "SEARCH")
                body["query"] = value;
            else if (flag == "--key" && command == "SUGGEST")
                body["key"] = value;
            else if (flag == "--top-k" && (command == "SEARCH" || command == "SUGGEST")) {
                const auto k = number(value);
                if (!k || k > 100)
                    bad();
                body["top_k"] = k;
            } else if (flag == "--mode" && command == "SEARCH") {
                if (value != "or" && value != "and")
                    bad();
                body["mode"] = value;
            } else if (flag == "--summary-window" && command == "SEARCH") {
                window = number(value);
                body["summary_window"] = window;
            } else if (flag == "--summary-context" && command == "SEARCH") {
                context = number(value);
                body["summary_context"] = context;
            } else
                bad();
        }
        if (!config.port || !config.timeout_ms || config.timeout_ms > 86400000
            || (command == "SEARCH"
                && (!seen.count("--query") || !window || window > 4096 || context >= window))
            || (command == "SUGGEST" && !seen.count("--key")))
            bad();
        request.body = body.dump();
    } catch (const std::invalid_argument &error) {
        std::cerr << "cli.arguments: " << error.what() << '\n';
        return 2;
    } catch (const nlohmann::json::exception &) {
        std::cerr << "cli.arguments: 请求不是合法UTF-8 JSON\n";
        return 2;
    }
    const auto result = exchange(config, request);
    if (!result) {
        std::cerr << result.error().context << ": " << result.error().message << '\n';
        return result.error().context == "client.config"
                       || result.error().context == "client.request"
                   ? 2
                   : 1;
    }
    const auto &response = result.value();
    if (response.message_type == MessageType::error_response) {
        std::cerr << "cli.remote status=" << response.status << ": " << response.body << '\n';
        return 5;
    }
    std::cout.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
    std::cout << '\n';
    std::cout.flush();
    if (!std::cout) {
        std::cerr << "cli.output: JSON输出失败\n";
        return 3;
    }
    return 0;
}
} // namespace siftwing::cli
