/*
 * PROJECT : SIFTWING
 * FILE    : online_server_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 生产控制者部分启动回滚、通知失败与晚到发布者静止屏障
 */
#include "siftwing/net/online_server.hpp"
#include "../support/online_server_fixture.hpp"
#include <atomic>
#include <fcntl.h>
#include <sys/epoll.h>

namespace {
using namespace siftwing;
using namespace siftwing::net;
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}
const std::thread::id controller = std::this_thread::get_id();
std::atomic<int> allocation_countdown{0}, write_mode{0};
std::atomic<bool> entered{false}, release_write{false}, stopping{false};
std::atomic<int> borrowed_fd{-1};
std::atomic<unsigned int> injections{0}, wrong_worker_io{0}, gate_entries{0};
std::atomic<bool> worker_alloc_failure{false}, gate_workers{false};
std::unique_ptr<query::SearchService> service(const std::string &parent) {
    query::SearchServiceConfig config;
    config.parent = parent;
    auto loaded = query::SearchService::create(config);
    if (!loaded)
        throw std::runtime_error(loaded.error().context);
    return std::move(loaded).value();
}
std::unique_ptr<OnlineServer> server(const std::string &parent) {
    auto created = OnlineServer::create(service(parent));
    if (auto *error = std::get_if<std::error_code>(&created))
        throw std::system_error(*error);
    return std::move(std::get<std::unique_ptr<OnlineServer>>(created));
}
}
extern "C" {
void *__real__Znwm(std::size_t);
void *__wrap__Znwm(std::size_t n) {
    if (std::this_thread::get_id() == controller && allocation_countdown.load() > 0
        && allocation_countdown.fetch_sub(1) == 1) {
        ++injections;
        throw std::bad_alloc();
    }
    if (std::this_thread::get_id() != controller) {
        if (worker_alloc_failure.exchange(false)) {
            ++injections;
            throw std::bad_alloc();
        }
        if (gate_workers.load()) {
            ++gate_entries;
            while (gate_workers.load())
                std::this_thread::yield();
        }
    }
    return __real__Znwm(n);
}
ssize_t __real_send(int, const void *, std::size_t, int);
ssize_t __wrap_send(int fd, const void *p, std::size_t n, int flags) {
    if (std::this_thread::get_id() != controller)
        ++wrong_worker_io;
    return __real_send(fd, p, n, flags);
}
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __wrap_recv(int fd, void *p, std::size_t n, int flags) {
    if (std::this_thread::get_id() != controller)
        ++wrong_worker_io;
    return __real_recv(fd, p, n, flags);
}
ssize_t __real_write(int, const void *, std::size_t);
ssize_t __wrap_write(int fd, const void *p, std::size_t n) {
    // 生产控制者/Reactor通知都在controller，Reactor不写eventfd；该静态调用来自worker发布。
    if (n == 8 && std::this_thread::get_id() != controller) {
        const int mode = write_mode.exchange(0);
        if (mode == 1) {
            ++injections;
            errno = EIO;
            return -1;
        }
        if (mode == 2) {
            borrowed_fd.store(fd);
            entered.store(true);
            while (!release_write.load())
                std::this_thread::yield();
        }
    }
    return __real_write(fd, p, n);
}
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const std::string parent = argv[1];
    const auto baseline = fd_count();
    // 对控制线程前24个new逐位置单次故障；被分配路径是否到达由原包装回执确定。
    for (int position = 1; position <= 24; ++position) {
        auto ready = service(parent);
        allocation_countdown.store(position);
        auto created = OnlineServer::create(std::move(ready));
        allocation_countdown.store(0);
        if (auto *error = std::get_if<std::error_code>(&created))
            check(error->value() == ENOMEM,
                "startup allocation failure mapped without partial server");
        else {
            auto running = std::move(std::get<std::unique_ptr<OnlineServer>>(created));
            siftwing::test::OnlineServerDriver drive(*running);
            check(!running->stop(), "unreached allocation checkpoint safely stops");
        }
        check(fd_count() == baseline, "partial startup rollback preserves fd baseline");
    }
    check(
        injections.load() == 24, "every requested control allocation checkpoint actually injected");
    {
        auto running = server(parent);
        siftwing::test::OnlineServerDriver drive(*running);
        check(running->run_once(-1).value() == EINVAL && running->port() != 0,
            "bad timeout preserves running service");
        write_mode.store(1);
        auto peer = client(running->port());
        send_all(peer.get(), wire(3, 77, "{\"query\":\"red\"}"));
        std::error_code failure;
        await([&] {
            failure = running->run_once(1);
            return bool(failure);
        });
        check(failure.value() == EIO && running->report().stopped,
            "accepted notification failure stops whole service");
        check(running->stop() == failure && running->port() == 0,
            "stopped first cause stable and no listener");
        check(running->report().workers.succeeded == 1,
            "no retry after accepted notification failure");
    }
    {
        auto running = server(parent);
        siftwing::test::OnlineServerDriver drive(*running);
        entered.store(false);
        release_write.store(false);
        stopping.store(false);
        write_mode.store(2);
        auto peer = client(running->port());
        send_all(peer.get(), wire(3, 88, "{\"query\":\"red\"}"));
        await([&] {
            (void)running->run_once(1);
            return entered.load();
        });
        std::atomic<bool> retained{false};
        std::thread releaser([&] {
            while (!stopping.load())
                std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            retained.store(::fcntl(borrowed_fd.load(), F_GETFD) >= 0);
            release_write.store(true);
        });
        stopping.store(true);
        const auto error = running->stop();
        releaser.join();
        check(!error && retained.load(),
            "stop retains eventfd through late complete publication call");
        check(::fcntl(borrowed_fd.load(), F_GETFD) == -1 && errno == EBADF,
            "complete publisher barrier then closes notification fd");
        const auto report = running->report();
        check(report.stopped && report.workers.succeeded == 1 && report.workers.failed == 0,
            "exactly one completed worker before release");
        for (const auto &sub : {report.reactor})
            check(sub.active == 0 && sub.state == ReactorState::stopped,
                "single Reactor resources closed");
    }
    {
        auto running = server(parent);
        siftwing::test::OnlineServerDriver drive(*running);
        worker_alloc_failure.store(true);
        auto peer = client(running->port());
        send_all(peer.get(), wire(3, 99, "{\"query\":\"red\"}"));
        await([&] {
            (void)running->run_once(1);
            return running->report().workers.succeeded == 1;
        });
        const auto expected = wire(0xffff, 99, "{\"message\":\"internal error\"}", 4);
        check(receive(peer.get(), expected.size()) == expected,
            "worker allocation exception maps one fixed safe error");
        check(!running->stop() && !worker_alloc_failure.load(),
            "actual worker exception isolated from whole service");
    }
    {
        OnlineServerConfig config;
        config.connection.max_connections = 128;
        config.connection.completion_capacity = 1;
        auto created = OnlineServer::create(service(parent), config);
        auto running = std::move(std::get<std::unique_ptr<OnlineServer>>(created));
        siftwing::test::OnlineServerDriver drive(*running);
        gate_entries.store(0);
        gate_workers.store(true);
        std::array<UniqueFd, 4> peers;
        for (auto &peer : peers) {
            peer = client(running->port());
            send_all(peer.get(), wire(3, 100, "{\"query\":\"red\"}"));
        }
        await([&] {
            (void)running->run_once(1);
            return running->report().reactor.requests == 4 && gate_entries.load() == 4;
        });
        // 所有任务已交接；停止推进唯一调用线程，让真实worker确定填满容量1的结果队列。
        const auto saved_progress = io_progress;
        io_progress = nullptr;
        gate_workers.store(false);
        await([&] {
            return running->report().workers.succeeded == 4;
        });
        check(running->report().first_error.value() == ENOBUFS,
            "actual full completion queue records fatal cause");
        io_progress = saved_progress;
        check(running->run_once(1).value() == ENOBUFS && running->report().stopped,
            "full result stops whole service without retry");
        check(running->report().workers.succeeded == 4, "all four jobs execute only once");
    }
    check(fd_count() == baseline && injections.load() == 26 && wrong_worker_io.load() == 0,
        "no fd leak and one exact notification injection");
    std::cout << "online native checks=" << checks << " injections=" << injections.load() << '\n';
}
