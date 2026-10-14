/*
 * PROJECT : SIFTWING
 * FILE    : multi_reactor_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 独立协议黄金字节、真实多Sub轮询与系统调用线程归属观测
 */
#include "../support/multi_reactor_fixture.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <map>

namespace {
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool value, const char *label) {
    require(value, label, checks);
}
struct Record {
    int fd = -1;
    std::thread::id main, owner, closer;
    std::uint64_t token = 0;
    unsigned int reads = 0, writes = 0, closes = 0;
};
std::mutex observer;
std::array<Record, 1024> records{};
std::size_t used = 0;
std::map<int, std::size_t> live;
std::map<std::thread::id, std::uint64_t> last_token;
bool valid = true;
void observe_io(int fd, bool write) {
    const std::lock_guard<std::mutex> lock(observer);
    auto item = live.find(fd);
    if (item == live.end())
        return;
    auto &record = records[item->second];
    valid = valid && record.owner == std::this_thread::get_id() && record.owner != record.main;
    if (write)
        ++record.writes;
    else
        ++record.reads;
}
} // namespace
extern "C" {
int __real_accept4(int, sockaddr *, socklen_t *, int);
int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags) {
    const int result = __real_accept4(fd, address, size, flags);
    if (result >= 0) {
        const std::lock_guard<std::mutex> lock(observer);
        if (used == records.size())
            std::exit(2);
        records[used].fd = result;
        records[used].main = std::this_thread::get_id();
        live[result] = used++;
    }
    return result;
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int fd, int op, int target, epoll_event *event) {
    const int result = __real_epoll_ctl(fd, op, target, event);
    if (result == 0) {
        const std::lock_guard<std::mutex> lock(observer);
        auto item = live.find(target);
        if (item != live.end()) {
            auto &record = records[item->second];
            const auto thread = std::this_thread::get_id();
            if (op == EPOLL_CTL_ADD) {
                record.owner = thread;
                record.token = event->data.u64;
                valid = valid && thread != record.main && record.token > last_token[thread];
                last_token[thread] = record.token;
            } else
                valid = valid && record.owner == thread;
        }
    }
    return result;
}
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __wrap_recv(int fd, void *bytes, std::size_t size, int flags) {
    observe_io(fd, false);
    return __real_recv(fd, bytes, size, flags);
}
ssize_t __real_send(int, const void *, std::size_t, int);
ssize_t __wrap_send(int fd, const void *bytes, std::size_t size, int flags) {
    observe_io(fd, true);
    return __real_send(fd, bytes, size, flags);
}
int __real_close(int);
int __wrap_close(int fd) {
    {
        const std::lock_guard<std::mutex> lock(observer);
        auto item = live.find(fd);
        if (item != live.end()) {
            auto &record = records[item->second];
            ++record.closes;
            record.closer = std::this_thread::get_id();
            if (record.owner != std::thread::id{})
                valid = valid && record.closer == record.owner;
            live.erase(item);
        }
    }
    return __real_close(fd);
}
}
int main() {
    const auto baseline = fd_count();
    // 固定连接归属的独立参考是连接建立顺序mod N；同一peer的所有请求只在该Sub计数。
    for (unsigned int cycle = 0; cycle < 4; ++cycle) {
        {
            const std::lock_guard<std::mutex> lock(observer);
            last_token.clear();
        }
        const auto family = cycle % 2 ? IpFamily::v6 : IpFamily::v4;
        const std::size_t count = cycle % 2 ? 3 : 2;
        ProtocolSubConfig config;
        config.read_byte_budget = 7;
        config.write_byte_budget = 5;
        config.io_attempt_budget = 2;
        config.handoff_budget = 2;
        MultiReactorFixture server(count, config);
        check(!server.start({family, family == IpFamily::v4 ? "127.0.0.1" : "::1", 0, 64, true}),
            "all targets ready before real Main listener");
        std::vector<UniqueFd> peers;
        for (std::size_t ordinal = 0; ordinal < 12; ++ordinal) {
            peers.push_back(client(server.port(), family));
            await([&] {
                return server.sub(ordinal % count).status().admitted == ordinal / count + 1;
            });
            for (std::size_t target = 0; target < count; ++target)
                check(server.sub(target).status().admitted
                          == ordinal / count + (target <= ordinal % count ? 1U : 0U),
                    "independent round robin reference");
            const auto id = 1000 * cycle + 10 * ordinal + 1;
            const auto first = wire(1, id);
            send_all(peers.back().get(), first.substr(0, 9));
            send_all(peers.back().get(), first.substr(9));
            check(receive(peers.back().get(), 26) == wire(0x8001, id),
                "fragmented PING exact PONG bytes");
        }
        for (std::size_t ordinal = 0; ordinal < peers.size(); ++ordinal) {
            const auto id = 1000 * cycle + 10 * ordinal + 2;
            std::string requests, expected;
            for (std::uint64_t n = 0; n < 3; ++n) {
                requests += wire(1, id + n);
                expected += wire(0x8001, id + n);
            }
            send_all(peers[ordinal].get(), requests);
            check(::shutdown(peers[ordinal].get(), SHUT_WR) == 0, "half-close request stream");
            check(receive(peers[ordinal].get(), expected.size()) == expected,
                "cached pipeline survives half-close with bounded writes");
        }
        for (std::size_t target = 0; target < count; ++target) {
            await([&] {
                return server.sub(target).status().responses == 48 / count
                       && server.sub(target).status().active == 0;
            });
            check(server.sub(target).status().requests == 48 / count,
                "fixed Sub owns every request on its connections");
        }
        auto bad = client(server.port(), family);
        send_all(bad.get(), wire(3, 9000 + cycle, "[]"));
        const auto expected = wire(0xffff, 9000 + cycle, R"({"message":"invalid request"})", 1);
        check(receive(bad.get(), expected.size()) == expected,
            "trusted bad Body preserves id and error policy");
        auto header = client(server.port(), family);
        auto broken = wire(1, 9900 + cycle);
        broken[0] = 'X';
        send_all(header.get(), broken);
        await([&] {
            char byte = 0;
            const auto received_bytes = ::recv(header.get(), &byte, 1, MSG_DONTWAIT);
            return received_bytes == 0 || (received_bytes < 0 && errno == ECONNRESET);
        });
        check(!server.stop() && !server.stop(), "idempotent whole-topology stop");
        const auto totals = server.totals();
        check(totals.accepted == 14 && totals.handed_off == 14 && totals.rejected == 0,
            "Main never reads business");
        for (std::size_t target = 0; target < count; ++target) {
            const auto status = server.sub(target).status();
            check(status.state == ProtocolSubState::joined && status.active == 0
                      && status.admitted == status.dropped && !status.first_error
                      && !status.cleanup_error,
                "each Sub final cleanup complete");
        }
        // 主I/O线程可能由OS重用thread::id，新的Sub注册空间独立，从1重新开始属于新实例。
    }
    {
        {
            const std::lock_guard<std::mutex> lock(observer);
            last_token.clear();
        }
        ProtocolSubConfig config;
        config.max_connections = 1;
        MultiReactorFixture server(3, config);
        check(!server.start(), "capacity topology");
        std::array<UniqueFd, 6> peers;
        for (std::size_t i = 0; i < 3; ++i) {
            peers[i] = client(server.port());
            await([&] {
                return server.sub(i).status().admitted == 1;
            });
        }
        for (std::size_t i = 3; i < 6; ++i) {
            peers[i] = client(server.port());
            await([&] {
                return server.sub(i % 3).status().rejected == 1;
            });
        }
        for (std::size_t i = 0; i < 3; ++i) {
            send_all(peers[i].get(), wire(1, 800 + i));
            check(receive(peers[i].get(), 26) == wire(0x8001, 800 + i),
                "capacity rejection does not stop existing connection");
        }
        check(!server.stop(), "capacity cleanup");
        for (std::size_t i = 0; i < 3; ++i) {
            const auto status = server.sub(i).status();
            check(status.received == 2 && status.admitted == 1 && status.rejected == 1
                      && status.dropped == 1,
                "slot full rejected by target without migration");
        }
    }
    {
        {
            const std::lock_guard<std::mutex> lock(observer);
            last_token.clear();
        }
        MultiReactorFixture server(2);
        check(!server.start(), "fd reuse topology");
        for (std::uint64_t id = 1; id <= 24; ++id) {
            auto peer = client(server.port());
            send_all(peer.get(), wire(1, 10000 + id));
            check(receive(peer.get(), 26) == wire(0x8001, 10000 + id),
                "new registration survives kernel fd reuse");
            (void)peer.close();
            await([&] {
                return server.sub(0).status().active + server.sub(1).status().active == 0;
            });
        }
        check(!server.stop(), "fd reuse cleanup");
    }
    {
        const std::lock_guard<std::mutex> lock(observer);
        check(valid && live.empty(), "all observed I/O and registration stay on owner Sub");
        bool reused = false;
        for (std::size_t i = 0; i < used; ++i) {
            const auto &record = records[i];
            check(record.closes == 1, "each accepted server fd closed once");
            if (record.owner != std::thread::id{})
                check(record.owner != record.main && record.closer == record.owner,
                    "Main never closes committed connection");
            for (std::size_t j = 0; j < i; ++j)
                if (records[j].fd == record.fd)
                    reused = true;
        }
        check(reused, "actual numeric fd reuse observed");
    }
    check(fd_count() == baseline, "no descriptor growth");
    std::cout << "multi reactor checks=" << checks << '\n';
}
