/*
 * PROJECT : SIFTWING
 * FILE    : main.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 显式快照与固定资源的loopback在线SEARCH/SUGGEST入口
 */
#include "siftwing/net/online_server.hpp"
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <set>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop_signal(int) {
    interrupted = 1;
}
std::uint64_t number(const std::string &text, std::uint64_t cap, bool zero = false) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || std::to_string(result) != text || result > cap || (!zero && !result))
        throw std::invalid_argument("invalid integer");
    return result;
}
}
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout
            << "用法：siftwing-server --snapshot <父目录> --version <uint64> [选项]\n"
               "仅监听127.0.0.1，支持PING、SERVER_INFO及SEARCH/SUGGEST；固定快照，停止限时排空网络输出。\n"
               "--port 0..65535（默认0） --subs 1..64（默认2） --workers 1..64（默认4）\n"
               "--task-capacity 1..1024（默认128） --run-ms 0..86400000（默认0，等停止信号）\n"
               "--shutdown-ms 0..86400000（默认1000，网络排空期限；仍须安全等待worker退出）\n"
               "--tokenizer english|cppjieba --stopwords 文件 --dictionary 文件 --model 文件\n"
               "--user-dictionary 文件 --hmm on|off（中文资源须与搜索建库相容）\n"
               "--max-query-bytes 1..65536 --max-top-k 1..100 --max-work 1..50000000\n"
               "--max-output-bytes 1..1048576 --max-snapshot-bytes 1..536870912 --max-resource-bytes 1..16777216\n";
        std::cout.flush();
        return std::cout ? 0 : 3;
    }
    siftwing::query::SearchServiceConfig service;
    siftwing::net::OnlineServerConfig config;
    std::uint64_t duration = 0;
    std::int64_t shutdown_ms = 1000;
    try {
        std::set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (!seen.insert(flag).second || ++i == argc)
                throw std::invalid_argument("duplicate/missing option");
            const std::string value = argv[i];
            if (value.empty())
                throw std::invalid_argument("empty option");
            if (flag == "--snapshot")
                service.parent = value;
            else if (flag == "--version")
                service.version = number(value, UINT64_MAX, true);
            else if (flag == "--port")
                config.listen.port = static_cast<std::uint16_t>(number(value, 65535, true));
            else if (flag == "--subs")
                config.sub_count = static_cast<std::size_t>(number(value, 64));
            else if (flag == "--workers")
                config.workers = static_cast<std::size_t>(number(value, 64));
            else if (flag == "--task-capacity")
                config.task_capacity = static_cast<std::size_t>(number(value, 1024));
            else if (flag == "--run-ms")
                duration = number(value, 86400000, true);
            else if (flag == "--shutdown-ms")
                shutdown_ms = static_cast<std::int64_t>(number(value, 86400000, true));
            else if (flag == "--tokenizer")
                service.tokenizer = value;
            else if (flag == "--stopwords")
                service.stopwords = value;
            else if (flag == "--dictionary")
                service.dictionary = value;
            else if (flag == "--model")
                service.model = value;
            else if (flag == "--user-dictionary")
                service.user = value;
            else if (flag == "--hmm" && (value == "on" || value == "off"))
                service.hmm = value == "on";
            else if (flag == "--max-query-bytes")
                service.query_bytes = number(value, 65536);
            else if (flag == "--max-top-k")
                service.max_k = number(value, 100);
            else if (flag == "--max-work")
                service.work = number(value, 50000000);
            else if (flag == "--max-output-bytes")
                service.output_bytes = number(value, 1048576);
            else if (flag == "--max-snapshot-bytes")
                service.snapshot_bytes = number(value, 536870912);
            else if (flag == "--max-resource-bytes")
                service.resource_bytes = number(value, 16777216);
            else
                throw std::invalid_argument("unknown option");
        }
        if (!seen.count("--snapshot") || !seen.count("--version")
            || (service.tokenizer != "english" && service.tokenizer != "cppjieba")
            || (service.tokenizer == "english"
                && (seen.count("--hmm") || !service.dictionary.empty() || !service.model.empty()
                    || !service.user.empty()))
            || (service.tokenizer == "cppjieba"
                && (service.dictionary.empty() || service.model.empty())))
            throw std::invalid_argument("required/incompatible option");
    } catch (...) {
        std::cerr << "服务参数非法，使用--help查看用法。\n";
        return 2;
    }
    try {
        // 只在进程入口装载；worker请求不携带路径，也不重新读取快照或资源。
        auto loaded = siftwing::query::SearchService::create(std::move(service));
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
