/*
 * PROJECT : SIFTWING
 * FILE    : main.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 有限轮次的loopback协议探针，验证单循环PING/错误响应
 */
#include "siftwing/net/protocol_loop.hpp"
#include <charconv>
#include <iostream>
#include <string_view>

namespace {
bool number(std::string_view text, unsigned int &value, unsigned int maximum) {
    if (text.empty())
        return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value <= maximum;
}
} // namespace
int main(int argc, char **argv) {
    unsigned int port = 0, turns = 1000, timeout = 10;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout
            << "用法：siftwing-protocol-probe [端口0..65535 [轮次1..1000000 [等待毫秒0..1000]]]\n"
               "仅监听127.0.0.1，支持PING/PONG和协议错误，按轮次退出；不执行搜索业务。\n";
        std::cout.flush();
        return std::cout ? 0 : 3;
    }
    if (argc > 4 || (argc >= 2 && !number(argv[1], port, 65535))
        || (argc >= 3 && (!number(argv[2], turns, 1000000) || turns == 0))
        || (argc >= 4 && !number(argv[3], timeout, 1000))) {
        std::cerr << "参数非法，使用--help查看用法。\n";
        return 2;
    }
    try {
        siftwing::net::ListenConfig listen;
        listen.port = static_cast<std::uint16_t>(port);
        auto created = siftwing::net::ProtocolLoop::create(listen);
        if (auto *error = std::get_if<std::error_code>(&created)) {
            std::cerr << "创建协议探针失败，errno=" << error->value() << '\n';
            return 3;
        }
        auto &loop = *std::get<std::unique_ptr<siftwing::net::ProtocolLoop>>(created);
        std::cout << "PORT " << loop.port() << '\n';
        std::cout.flush();
        if (!std::cout)
            return 3;
        std::size_t accepted = 0, completed = 0;
        for (unsigned int i = 0; i < turns; ++i) {
            const auto step = loop.run_once(static_cast<int>(timeout));
            accepted += step.accepted;
            completed += step.completed_responses;
            if (step.stop == siftwing::net::LoopStop::failed) {
                std::cerr << "协议循环失败，errno=" << step.error.value() << '\n';
                return 3;
            }
        }
        const auto error = loop.close();
        if (error) {
            std::cerr << "资源关闭失败，errno=" << error.value() << '\n';
            return 3;
        }
        std::cout << "accepted=" << accepted << " completed=" << completed << '\n';
        std::cout.flush();
        return std::cout ? 0 : 3;
    } catch (...) {
        std::cerr << "本地资源或库异常，协议探针退出。\n";
        return 3;
    }
}
