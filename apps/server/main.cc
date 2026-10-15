/*
 * PROJECT : SIFTWING
 * FILE    : main.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 严格配置准入与固定资源的在线SEARCH/SUGGEST入口
 */
#include "siftwing/config/server_settings.hpp"
#include <chrono>
#include <csignal>
#include <iostream>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop_signal(int) {
    interrupted = 1;
}

}
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout
            << "用法：siftwing-server --snapshot <父目录> --version <uint64> [选项]\n"
               "默认监听127.0.0.1，可显式指定数值IPv4/IPv6；支持PING、SERVER_INFO及SEARCH/SUGGEST；固定快照，停止限时排空网络输出。\n"
               "--config JSON文件（schema_version=1；默认值→文件→CLI；启动时读取）\n"
               "--listen-address 数值IP --port 0..65535（默认0） --subs 1..64（默认2） --workers 1..64（默认4）\n"
               "--task-capacity 1..1024（默认128） --run-ms 0..86400000（默认0，等停止信号）\n"
               "--shutdown-ms 0..86400000（默认1000，网络排空期限；仍须安全等待worker退出）\n"
               "--idle-timeout-ms 0..86400000 --request-timeout-ms 0..86400000（默认均0关闭）\n"
               "--cache-capacity 0..4096（默认0关闭） --cache-bytes 1..67108864 --cache-max-value-bytes 1..1048576\n"
               "--handoff-capacity/--completion-capacity/--max-connections 1..1024（每Sub）\n"
               "--handoff-budget/--completion-budget/--io-attempt-budget/--accept-attempt-budget 1..1024\n"
               "--read-byte-budget/--write-byte-budget 1..1048600 --max-body-bytes 2..1048576\n"
               "--output-low-bytes/--output-high-bytes/--output-hard-bytes 0..1048600（全0自动）\n"
               "--backlog 1..2147483647 --tcp-no-delay on|off --reuse-address on|off（默认off）\n"
               "--tokenizer english|cppjieba --stopwords 文件 --dictionary 文件 --model 文件\n"
               "--user-dictionary 文件 --hmm on|off（中文资源须与搜索建库相容）\n"
               "--max-query-bytes 1..65536 --max-top-k 1..100 --max-work 1..50000000\n"
               "--max-output-bytes 1..1048576 --max-snapshot-bytes 1..536870912 --max-resource-bytes 1..16777216\n";
        std::cout.flush();
        return std::cout ? 0 : 3;
    }
    try {
        std::vector<std::string> arguments(argv + 1, argv + argc);
        auto configured = siftwing::config::load_server_settings(arguments);
        if (!configured) {
            std::cerr << "配置加载失败，context=" << configured.error().context << "，"
                      << configured.error().message << '\n';
            return 2;
        }
        auto settings = std::move(configured).value();
        const auto config = siftwing::config::online_config(settings);
        const auto duration = settings.run_ms;
        const auto shutdown_ms = settings.shutdown_ms;
        // 只在进程入口装载；worker请求不携带路径，也不重新读取快照或资源。
        auto loaded = siftwing::query::SearchService::create(std::move(settings.service));
        if (!loaded) {
            std::cerr << "服务装载失败，context=" << loaded.error().context << '\n';
            return 1;
        }
        auto created = siftwing::net::OnlineServer::create(std::move(loaded).value(), config);
        if (auto *error = std::get_if<std::error_code>(&created)) {
            std::cerr << "服务启动失败，errno=" << error->value() << '\n';
            return 1;
        }
        auto server = std::move(std::get<std::unique_ptr<siftwing::net::OnlineServer>>(created));
        if (std::signal(SIGINT, stop_signal) == SIG_ERR
            || std::signal(SIGTERM, stop_signal) == SIG_ERR)
            throw std::runtime_error("signal setup failed");
        std::cout << "PORT " << server->port() << '\n';
        std::cout.flush();
        if (!std::cout)
            return 3;
        const auto begin = std::chrono::steady_clock::now();
        while (!interrupted) {
            if (duration
                && std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - begin)
                           .count()
                       >= static_cast<std::int64_t>(duration))
                break;
            if (const auto error = server->run_once(20)) {
                std::cerr << "服务运行失败，errno=" << error.value() << '\n';
                return 1;
            }
        }
        if (const auto error = server->shutdown(shutdown_ms)) {
            std::cerr << "服务停止失败，errno=" << error.value() << '\n';
            return 1;
        }
        const auto report = server->report();
        std::size_t requests = 0, responses = 0, active = 0;
        for (const auto &sub : report.subs) {
            requests += sub.requests;
            responses += sub.responses;
            active += sub.active;
        }
        std::cout << "{\"accepted\":" << report.accepted << ",\"requests\":" << requests
                  << ",\"responses\":" << responses << ",\"active\":" << active
                  << ",\"workers_succeeded\":" << report.workers.succeeded
                  << ",\"workers_failed\":" << report.workers.failed << "}\n";
        std::cout.flush();
        return std::cout ? 0 : 3;
    } catch (...) {
        std::cerr << "服务本地资源或库异常。\n";
        return 4;
    }
}
