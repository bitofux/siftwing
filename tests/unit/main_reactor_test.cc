/*
 * PROJECT : SIFTWING
 * FILE    : main_reactor_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 真实TCP独立参考验证轮询、预算、满不改投与业务字节保持
 */
#include "siftwing/net/main_reactor.hpp"
#include <arpa/inet.h>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool value, const char *label) {
    ++checks; if (!value) { std::cerr << "main reactor failed: " << label << '\n'; std::exit(1); }
}
UniqueFd notification() {
    auto result = create_eventfd_wakeup(); require(std::holds_alternative<UniqueFd>(result), "eventfd");
    return std::move(std::get<UniqueFd>(result));
}
UniqueFd client(std::uint16_t port, char payload) {
    UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)); require(static_cast<bool>(fd), "client socket");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    require(::connect(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "connect TCP");
    require(::send(fd.get(), &payload, 1, MSG_NOSIGNAL) == 1, "send business byte before handoff"); return fd;
}
std::unique_ptr<MainReactor> main_loop(const std::vector<MainReactorTarget> &targets, std::size_t budget = 1) {
    MainReactorConfig config; config.accept_attempt_budget = budget; config.tcp_no_delay = true;
    auto result = MainReactor::create(targets, {}, config);
    require(std::holds_alternative<std::unique_ptr<MainReactor>>(result), "complete Main");
    return std::move(std::get<std::unique_ptr<MainReactor>>(result));
}
} // namespace
int main() {
    {
        FdHandoffQueue q0(2), q1(2), q2(2); auto n0 = notification(), n1 = notification(), n2 = notification();
        std::array<FdHandoffQueue *, 3> queues{&q0,&q1,&q2};
        auto main = main_loop({{&q0,n0.get()},{&q1,n1.get()},{&q2,n2.get()}});
        const auto port = main->port(); require(port != 0, "actual port");
        require(main->run_once(-1).stop == MainReactorStop::invalid_timeout && main->run_once(101).error.value() == EINVAL,
            "no infinite wait and invalid timeout preserves");
        require(main->run_once(0).stop == MainReactorStop::idle, "empty listener");
        for (unsigned int id = 0; id < 18; ++id) {
            const char expected = static_cast<char>(id + 1); auto peer = client(port, expected);
            const auto step = main->run_once(100);
            require(step.stop == MainReactorStop::progress && step.accept_attempts == 1 && step.accepted == 1 &&
                step.handed_off == 1 && step.rejected == 0 && step.accept_budget_exhausted, "one bounded accept");
            for (std::size_t target = 0; target < queues.size(); ++target)
                require(queues[target]->status().size == (target == id % queues.size() ? 1U : 0U), "independent modulo reference");
            auto owner = queues[id % queues.size()]->try_pop(); require(owner.has_value(), "fixed target owns");
            char actual = 0;
            require(::recv(owner->get(), &actual, 1, MSG_DONTWAIT) == 1 && actual == expected, "Main never consumes business data");
            require((::fcntl(owner->get(), F_GETFL) & O_NONBLOCK) && (::fcntl(owner->get(), F_GETFD) & FD_CLOEXEC), "accepted atomic flags");
            int enabled = 0; socklen_t size = sizeof(enabled);
            require(::getsockopt(owner->get(), IPPROTO_TCP, TCP_NODELAY, &enabled, &size) == 0 && enabled == 1, "configured TCP_NODELAY");
            require(!owner->close(), "target cleanup");
        }
        require(!main->close() && !main->close() && main->port() == 0, "idempotent cleanup");
        const auto terminal = main->run_once(0);
        require(terminal.stop == MainReactorStop::closed && terminal.accept_attempts == 0, "closed has zero I/O");
        require(q0.status().closed && q1.status().closed && q2.status().closed, "stop closes publishing endpoints");
        require(!notify_eventfd(n0.get()) && !notify_eventfd(n1.get()), "Main never closes borrowed wakeup");
    }
    {
        FdHandoffQueue q0(1), q1(1); auto n0 = notification(), n1 = notification();
        UniqueFd seed(::open("/dev/null", O_RDONLY | O_CLOEXEC)); require(static_cast<bool>(seed), "queue seed");
        require(q0.try_push(seed) == FdHandoffResult::accepted, "q0 full before accept");
        auto main = main_loop({{&q0,n0.get()},{&q1,n1.get()}});
        std::array<UniqueFd, 5> peers;
        for (std::size_t i = 0; i < 3; ++i) {
            peers[i] = client(main->port(), 'x'); const auto result = main->run_once(100);
            require(result.accepted == 1 && result.rejected == (i == 1 ? 0U : 1U) && result.handed_off == (i == 1 ? 1U : 0U),
                "full rejects without reroute and advances cursor");
            require(q0.status().size == 1 && q1.status().size == (i == 0 ? 0U : 1U), "fixed ownership after rejection");
        }
        auto old = q0.try_pop(); require(old && !old->close(), "free q0");
        peers[3] = client(main->port(), 'y'); auto fourth = main->run_once(100);
        require(fourth.rejected == 1 && q0.status().size == 0, "q1 full must not reroute to free q0");
        peers[4] = client(main->port(), 'z'); auto fifth = main->run_once(100);
        require(fifth.handed_off == 1 && q0.status().size == 1, "cursor advanced even on full");
        q1.close(); const auto failure = main->run_once(0);
        require(failure.stop == MainReactorStop::failed && failure.error.value() == ECANCELED && failure.accepted == 0,
            "closed target stops whole Main without silent shrink");
        require(q0.status().size == 1 && q1.status().size == 1, "Main never drains submitted owners on failure");
        while (auto item = q0.try_pop()) require(!item->close(), "target q0 cleanup");
        while (auto item = q1.try_pop()) require(!item->close(), "target q1 cleanup");
    }
    {
        FdHandoffQueue queue(8); auto wakeup = notification(); auto main = main_loop({{&queue,wakeup.get()}}, 3);
        std::array<UniqueFd, 5> peers;
        for (auto &peer : peers) peer = client(main->port(), 'b');
        auto first = main->run_once(100);
        require(first.accepted == 3 && first.accept_attempts == 3 && first.accept_budget_exhausted, "budget caps ready backlog");
        auto second = main->run_once(100);
        require(second.accepted == 2 && second.accept_attempts == 3, "LT resumes remaining backlog then EAGAIN");
        require(!main->request_stop(), "self stop notification");
        require(main->run_once(0).stop == MainReactorStop::closed, "stop checked before accept");
        require(queue.status().size == 5, "stop leaves submitted owners for target");
        while (auto item = queue.try_pop()) require(!item->close(), "target drains after stopped");
    }
    std::cout << "main reactor checks=" << checks << '\n';
}
