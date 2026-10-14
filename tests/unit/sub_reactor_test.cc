/*
 * PROJECT : SIFTWING
 * FILE    : sub_reactor_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 真实线程生命周期、并发停止和析构回收，Release仍执行合同检查
 */
#include "siftwing/net/sub_reactor.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <type_traits>

namespace {
using namespace siftwing::net;
unsigned int checks = 0;
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "sub reactor failed: " << label << '\n';
        std::exit(1);
    }
}
std::size_t fd_count() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
static_assert(!std::is_copy_constructible_v<SubReactor>);
static_assert(!std::is_move_constructible_v<SubReactor>);
static_assert(noexcept(std::declval<SubReactor &>().request_stop()));
} // namespace
int main() {
    const auto baseline = fd_count();
    {
        SubReactor idle;
        require(!idle.join() && idle.status().state == SubReactorState::idle, "join before start");
        idle.request_stop();
        idle.request_stop();
        require(idle.start() == std::errc::operation_canceled, "stop before start cancels");
        require(idle.status().state == SubReactorState::joined, "no thread after cancellation");
        require(idle.start().value() == EALREADY, "one shot after cancellation");
    }
    // 真实epoll等待没有eventfd；请求返回与join分离，用宽松外部期限排除永久等待。
    for (int cycle = 0; cycle < 24; ++cycle) {
        SubReactor reactor;
        require(!reactor.start(), "real thread ready");
        require(reactor.status().state == SubReactorState::running, "ready means initialized");
        require(reactor.start().value() == EALREADY, "duplicate start rejected");
        const auto begin = std::chrono::steady_clock::now();
        std::atomic<bool> observing{true};
        std::atomic<bool> valid{true};
        std::thread observer([&] {
            while (observing.load()) {
                const auto status = reactor.status();
                if (status.first_error || status.cleanup_error
                    || status.state == SubReactorState::idle)
                    valid.store(false);
                std::this_thread::yield();
            }
        });
        std::array<std::thread, 4> stoppers;
        for (auto &stopper : stoppers)
            stopper = std::thread([&] {
                for (int attempt = 0; attempt < 100; ++attempt)
                    reactor.request_stop();
            });
        for (auto &stopper : stoppers)
            stopper.join();
        require(!reactor.join(), "join after concurrent stop");
        observing.store(false);
        observer.join();
        require(valid.load(), "concurrent status remains consistent");
        require(std::chrono::steady_clock::now() - begin < std::chrono::seconds(3),
            "finite stop observation under test scheduling");
        const auto status = reactor.status();
        require(status.state == SubReactorState::joined && status.stop_requested,
            "joined and requested are separate observations");
        require(!reactor.join() && reactor.start().value() == EALREADY, "repeat join no restart");
        require(fd_count() == baseline, "no fd growth across cycles");
    }
    // 固定两个Sub的控制者必须等全部ready；析构只回收本对象，不编排整个服务。
    {
        std::array<SubReactor, 2> reactors;
        for (auto &reactor : reactors)
            require(!reactor.start(), "two independent reactors ready");
        require(fd_count() == baseline + 2, "each ready reactor owns one epoll");
    }
    require(fd_count() == baseline, "destructors stop and join both reactors");
    std::cout << "checks=" << checks << " failures=0 cycles=24 concurrent_stoppers=4\n";
}
