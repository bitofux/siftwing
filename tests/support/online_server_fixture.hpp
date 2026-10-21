/*
 * PROJECT : SIFTWING
 * FILE    : online_server_fixture.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-21
 * BRIEF   : 测试调用线程等待peer/worker时显式推进唯一生产I/O循环
 */
#pragma once
#include "multi_reactor_fixture.hpp"
#include "siftwing/net/online_server.hpp"

namespace siftwing::test {
inline thread_local net::OnlineServer *driven_server = nullptr;
/**
 * @brief 借用同线程server至作用域退出；不创建线程、不替换业务结果或故障判定
 * @note server所有者必须先构造、后销毁。其他线程没有此thread_local入口，不能跨线程驱动。
 *     运行错误由既有测试检查报告/返回值；推进器不吞改报告，不在等待中重建服务。
 */
class OnlineServerDriver final {
    net::OnlineServer *previous_;
    void (*previous_progress_)();
    bool active_ = true;

public:
    explicit OnlineServerDriver(net::OnlineServer &server) noexcept
        : previous_(driven_server), previous_progress_(io_progress) {
        driven_server = &server;
        io_progress = [] {
            (void)driven_server->run_once(0);
        };
    }
    ~OnlineServerDriver() noexcept {
        release();
    }
    /** @brief 测试显式reset server前先撤本线程借用；重复不改变嵌套驱动器。 */
    void release() noexcept {
        if (active_) {
            driven_server = previous_;
            io_progress = previous_progress_;
            active_ = false;
        }
    }
    OnlineServerDriver(const OnlineServerDriver &) = delete;
    OnlineServerDriver &operator=(const OnlineServerDriver &) = delete;
};
} // namespace siftwing::test
