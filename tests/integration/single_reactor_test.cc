/*
 * PROJECT : SIFTWING
 * FILE    : single_reactor_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-21
 * BRIEF   : 总连接预算、控制token隔离、调用线程主动推进及注册失败回滚
 */
#include "../support/online_server_fixture.hpp"
#include "siftwing/query/search_service.hpp"
#include <sys/epoll.h>

namespace {
using namespace siftwing;
using namespace siftwing::net;
using namespace siftwing::test;
unsigned checks = 0;
int fail_add = 0;
unsigned failed_adds = 0;
void check(bool ok, const char *why) {
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
std::unique_ptr<query::SearchService> service(const char *parent) {
    query::SearchServiceConfig config;
    config.parent = parent;
    auto made = query::SearchService::create(config);
    if (!made)
        throw std::runtime_error(made.error().context);
    return std::move(made).value();
}
std::unique_ptr<OnlineServer> server(const char *parent, OnlineServerConfig config = {}) {
    auto made = OnlineServer::create(service(parent), config);
    if (const auto *error = std::get_if<std::error_code>(&made))
        throw std::system_error(*error);
    return std::move(std::get<std::unique_ptr<OnlineServer>>(made));
}
}
extern "C" {
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int fd, int op, int target, epoll_event *event) {
    if (op == EPOLL_CTL_ADD && fail_add > 0 && --fail_add == 0) {
        ++failed_adds;
        errno = EIO;
        return -1;
    }
    return __real_epoll_ctl(fd, op, target, event);
}
}
int main(int argc, char **argv) {
    try {
        if (argc != 3)
            return 2;
        const auto baseline = fd_count();
        {
            auto running = server(argv[1]);
            check(OnlineServerConfig{}.connection.max_connections == 256
                      && OnlineServerConfig{}.connection.completion_capacity == 256,
                "default budgets preserve former two times 128, not one Sub's budget");
            std::vector<UniqueFd> peers;
            for (std::size_t n = 0; n < 257; ++n) {
                peers.push_back(client(running->port()));
                await([&] {
                    check(!running->run_once(0), "finite accept round");
                    return running->report().accepted == n + 1;
                });
            }
            const auto full = running->report();
            check(full.accepted == 257 && full.reactor.admitted == 256 && full.reactor.active == 256
                      && full.reactor.rejected == 1,
                "global budget admits beyond old per-Sub 128 and rejects exactly at 256");
            send_all(peers.front().get(), wire(1, 7));
            send_all(peers[255].get(), wire(1, 8));
            // 不推进时没有后台Sub替调用者收发；请求应仍在内核输入中。
            char byte{};
            check(::recv(peers.front().get(), &byte, 1, MSG_DONTWAIT) == -1
                      && (errno == EAGAIN || errno == EWOULDBLOCK)
                      && running->report().reactor.requests == 0,
                "no autonomous production I/O progress without calling run_once");
            OnlineServerDriver drive(*running);
            check(receive(peers.front().get(), 26) == wire(0x8001, 7)
                      && receive(peers[255].get(), 26) == wire(0x8001, 8),
                "connection tokens 1 and 256 do not collide with control token zero");
            check(running->metrics().connections.handed_off == 0
                      && running->metrics().queues.handoff == 0,
                "retired handoff metrics are zero, not relabeled direct admissions");
            check(!running->shutdown(0) && running->report().reactor.active == 0,
                "same-thread zero-duration shutdown closes all idle connections");
        }
        check(fd_count() == baseline, "global budget round leaves no descriptors");
        {
            OnlineServerConfig maximum;
            maximum.connection.max_connections = 65536;
            maximum.connection.completion_capacity = 65536;
            auto running = server(argv[1], maximum);
            check(!running->stop(), "old maximum aggregate budget can be configured and stopped");
        }
        for (int point = 1; point <= 3; ++point) {
            auto ready = service(argv[1]);
            fail_add = point;
            const auto made = OnlineServer::create(std::move(ready));
            fail_add = 0;
            const auto *error = std::get_if<std::error_code>(&made);
            check(error && error->value() == EIO && fd_count() == baseline,
                "wakeup/timer/listener ADD failures roll back actual owned resources");
        }
        check(failed_adds == 3, "all three startup registration failures reached");
        std::cout << checks << " single production Reactor checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
