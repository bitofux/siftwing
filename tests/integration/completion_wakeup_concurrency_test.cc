/*
 * PROJECT : SIFTWING
 * FILE    : completion_wakeup_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 多Sub与真实WorkerPool回投、逐客户端字节参考及线程I/O归属
 */
#include "../support/completion_fixture.hpp"
#include <sys/epoll.h>
#include <array>
namespace {
using namespace siftwing::test;
unsigned int checks = 0;
std::atomic<unsigned int> wrong_thread{0};
thread_local bool worker_execution = false;
void check(bool value, const char *why) {
    require(value, why, checks);
}
struct Job final : WorkerTask {
    std::unique_ptr<RoutedSearchTask> task;
    CompletionTarget target;
    std::atomic<unsigned int> &errors;
    Job(std::unique_ptr<RoutedSearchTask> &source,
        CompletionTarget destination,
        std::atomic<unsigned int> &bad)
        : task(std::move(source)), target(destination), errors(bad) {}
    void execute() override {
        worker_execution = true;
        // 测试worker只构造拥有响应并发布，完全不接触Connection/身份表/socket。
        try {
            auto output = completion(*task, task->payload()->request()->body);
            const auto receipt = try_publish_and_notify(*target.queue, output, target.wakeup_fd);
            if (receipt.publish != CompletionPublishResult::accepted || receipt.notification_error
                || output)
                ++errors; // 本实验容量覆盖最大并发，无full重试作为生产策略。
        } catch (...) {
            ++errors;
        }
        worker_execution = false;
    }
};
struct Submitter final : RoutedRequestSubmitter {
    WorkerPool pool{4, 1024};
    std::array<CompletionTarget, 2> targets{}; // 入场前由控制者写好，此后冻结。
    std::atomic<unsigned int> errors{0};
    TaskSubmitResult try_submit(std::unique_ptr<RoutedSearchTask> &source) override {
        const auto destination = source->route().reactor_id - 31;
        if (destination >= targets.size())
            return TaskSubmitResult::invalid_task;
        std::unique_ptr<WorkerTask> job =
            std::make_unique<Job>(source, targets[destination], errors);
        try {
            const auto receipt = pool.try_submit(job);
            if (receipt != TaskSubmitResult::accepted)
                source = std::move(static_cast<Job *>(job.get())->task);
            return receipt;
        } catch (...) {
            source = std::move(static_cast<Job *>(job.get())->task);
            throw;
        }
    }
};
}
// 静态链接观测覆盖生产socket封装和epoll控制调用；不外推DSO内部或其他进程。
extern "C" {
ssize_t __real_send(int, const void *, std::size_t, int);
ssize_t __wrap_send(int fd, const void *p, std::size_t n, int flags) {
    if (worker_execution)
        ++wrong_thread;
    return __real_send(fd, p, n, flags);
}
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __wrap_recv(int fd, void *p, std::size_t n, int flags) {
    if (worker_execution)
        ++wrong_thread;
    return __real_recv(fd, p, n, flags);
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event) {
    if (worker_execution)
        ++wrong_thread;
    return __real_epoll_ctl(epoll, op, fd, event);
}
}
int main() {
    const auto baseline = fd_count();
    for (unsigned int round = 0; round < 4; ++round) {
        Submitter sink;
        ProtocolSubConfig config;
        config.reactor_id = 31;
        config.request_submitter = &sink;
        config.max_body_bytes = 256;
        config.max_connections = 32;
        config.completion_budget = 1;
        config.completion_capacity = 64;
        config.read_byte_budget = 11;
        config.write_byte_budget = 7;
        config.io_attempt_budget = 1;
        ProtocolSubReactor first(config);
        ++config.reactor_id;
        ProtocolSubReactor second(config);
        check(!first.start() && !second.start(), "all Sub targets ready before worker routing");
        sink.targets[0] = std::get<CompletionTarget>(first.completion_target());
        sink.targets[1] = std::get<CompletionTarget>(second.completion_target());
        std::array<UniqueFd, 48> peers;
        for (std::size_t i = 0; i < peers.size(); ++i)
            peers[i] = attach(i % 2 ? second : first);
        std::atomic<unsigned int> client_errors{0};
        std::array<std::thread, 6> clients;
        for (std::size_t p = 0; p < clients.size(); ++p)
            clients[p] = std::thread([&, p] {
                try {
                    for (std::size_t i = p * 8; i < (p + 1) * 8; ++i) {
                        std::string input, expected;
                        for (std::uint64_t request = 1; request <= 12; ++request) {
                            // 每个连接使用相同关联号；客户端专属Body能独立发现跨连接串响应。
                            const auto body = "{\"client\":" + std::to_string(i)
                                              + ",\"request\":" + std::to_string(request) + "}";
                            input += wire(request % 2 ? 3 : 4, request, body);
                            expected += wire(request % 2 ? 0x8003 : 0x8004, request, body);
                        }
                        send_all(peers[i].get(), input);
                        if (receive(peers[i].get(), expected.size()) != expected)
                            ++client_errors;
                    }
                } catch (...) {
                    ++client_errors;
                }
            });
        for (auto &thread : clients)
            thread.join();
        await([&] {
            return first.status().responses + second.status().responses == 576;
        });
        check(client_errors == 0 && sink.errors == 0,
            "all clients match independent complete wire reference");
        for (const auto *sub : {&first, &second}) {
            const auto report = sub->status();
            check(report.requests == 288 && report.responses == 288
                      && report.completion_received == 288 && report.completion_applied == 288
                      && report.completion_stale == 0 && report.completion_rejected == 0,
                "per owner all accepted results applied exactly once");
        }
        check(!first.request_stop() && !second.request_stop(),
            "stop both consumers before worker barrier");
        await([&] {
            return first.status().state == ProtocolSubState::quiescing
                   && second.status().state == ProtocolSubState::quiescing;
        });
        sink.pool.join(); // Sub不会再调用submitter；所有worker完整发布/通知已经返回。
        check(sink.pool.report().succeeded == 576 && sink.pool.report().failed == 0,
            "pool observes exactly one execution per accepted request");
        first.release_publishers();
        second.release_publishers();
        check(!first.join() && !second.join(), "both notification fds survive through worker join");
    }
    check(wrong_thread == 0 && fd_count() == baseline,
        "workers never issue socket I/O or epoll control; no fd leak");
    std::cout << "completion wakeup concurrency checks=" << checks << '\n';
}
