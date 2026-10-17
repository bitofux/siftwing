/*
 * PROJECT : SIFTWING
 * FILE    : multi_reactor_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 并发真实协议请求、固定目标分配与对象存活期间的并发停止观测
 */
#include "../support/multi_reactor_fixture.hpp"

namespace {
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool value, const char *label) {
    require(value, label, checks);
}
}
int main() {
    const auto baseline = fd_count();
    for (unsigned int field = 0; field < 7; ++field) {
        ProtocolSubConfig config;
        switch (field) {
            case 0:
                config.handoff_capacity = 0;
                break;
            case 1:
                config.max_connections = 1025;
                break;
            case 2:
                config.max_body_bytes = 1;
                break;
            case 3:
                config.handoff_budget = 0;
                break;
            case 4:
                config.read_byte_budget = 0;
                break;
            case 5:
                config.write_byte_budget = frame_header_bytes + frame_max_body_bytes + 1;
                break;
            default:
                config.io_attempt_budget = 1025;
                break;
        }
        bool rejected = false;
        try {
            ProtocolSubReactor sub(config);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        check(rejected, "whole config precheck before thread/I/O");
    }
    {
        ProtocolSubReactor sub;
        check(
            std::holds_alternative<std::error_code>(sub.target()), "idle does not publish target");
        check(!sub.join(), "idle join does not consume start");
        check(!sub.request_stop(), "early stop without wakeup");
        check(sub.start().value() == ECANCELED && sub.start().value() == EALREADY,
            "stop before start is terminal");
    }
    for (unsigned int cycle = 0; cycle < 4; ++cycle) {
        ProtocolSubConfig config;
        config.read_byte_budget = 9;
        config.write_byte_budget = 3;
        config.handoff_budget = 3;
        config.io_attempt_budget = 2;
        MultiReactorFixture server(3, config);
        check(!server.start(), "real three Sub topology ready");
        check(server.start().value() == EALREADY, "topology start once");
        std::atomic<bool> valid{true}, observe{true};
        std::thread observer([&] {
            while (observe.load())
                for (std::size_t i = 0; i < server.size(); ++i) {
                    const auto status = server.sub(i).status();
                    if (status.first_error || status.cleanup_error
                        || status.active > config.max_connections
                        || status.responses > status.requests)
                        valid.store(false);
                }
        });
        std::array<std::thread, 3> clients;
        for (std::size_t writer = 0; writer < clients.size(); ++writer)
            clients[writer] = std::thread([&, writer] {
                try {
                    for (std::size_t ordinal = 0; ordinal < 20; ++ordinal) {
                        auto peer = client(server.port());
                        std::string requests, expected;
                        for (std::uint64_t n = 0; n < 4; ++n) {
                            const auto id = 10000 * cycle + 100 * writer + 4 * ordinal + n + 1;
                            requests += wire(1, id);
                            expected += wire(0x8001, id);
                        }
                        send_all(peer.get(), requests);
                        if (receive(peer.get(), expected.size()) != expected)
                            valid.store(false);
                    }
                } catch (...) {
                    valid.store(false);
                }
            });
        for (auto &thread : clients)
            thread.join();
        await([&] {
            std::size_t responses = 0;
            for (std::size_t i = 0; i < server.size(); ++i)
                responses += server.sub(i).status().responses;
            // Sub响应与Main累计由不同线程分别发布；响应完成不证明Main快照已追上。
            const auto totals = server.totals();
            return responses == 240 && totals.accepted == 60 && totals.handed_off == 60
                   && totals.rejected == 0;
        });
        const auto totals = server.totals();
        check(totals.accepted == 60 && totals.handed_off == 60 && totals.rejected == 0,
            "exactly once concurrent handoff");
        for (std::size_t i = 0; i < server.size(); ++i) {
            const auto status = server.sub(i).status();
            check(status.admitted == 20 && status.requests == 80 && status.responses == 80,
                "fixed modulo distribution independent of writer order");
        }
        // 停止期间有未完成半帧；允许立即丢弃，不以响应排空推断清理或join。
        std::array<UniqueFd, 8> incomplete;
        for (auto &peer : incomplete) {
            peer = client(server.port());
            send_all(peer.get(), wire(1, 90000).substr(0, 13));
        }
        await([&] {
            return server.totals().handed_off == 68;
        });
        std::array<std::thread, 8> stoppers;
        for (auto &thread : stoppers)
            thread = std::thread([&] {
                for (unsigned int n = 0; n < 8; ++n)
                    if (server.request_stop())
                        valid.store(false);
            });
        for (auto &thread : stoppers)
            thread.join();
        check(!server.stop(), "stop waits Main notifications then releases all targets");
        observe.store(false);
        observer.join();
        check(valid.load(), "concurrent requests/status/stop remain valid");
        std::size_t owned = 0, discarded = 0;
        for (std::size_t i = 0; i < server.size(); ++i) {
            const auto status = server.sub(i).status();
            owned += status.received;
            discarded += status.discarded;
            check(status.state == ProtocolSubState::joined && status.active == 0
                      && status.admitted == status.dropped,
                "Sub resources closed before joined");
            check(std::holds_alternative<std::error_code>(server.sub(i).target())
                      && !server.sub(i).request_stop(),
                "late control call cannot touch revoked fd");
        }
        check(owned + discarded == 68, "all committed sources accounted for at immediate stop");
    }
    check(fd_count() == baseline, "no descriptor growth across cycles");
    std::cout << "multi concurrency checks=" << checks << '\n';
}
