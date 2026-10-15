/*
 * PROJECT : SIFTWING
 * FILE    : v2_fault_concurrency_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 正式服务的同fd旧结果隔离及慢输出/半包下的并发业务
 * IMPLEMENTATION : 真实查询与socket；静态包装仅控制执行门、fd复用和EAGAIN。
 */
#include "siftwing/net/online_server.hpp"
#include "../support/multi_reactor_fixture.hpp"
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/epoll.h>

namespace {
using namespace siftwing;
using namespace siftwing::net;
using namespace siftwing::concurrency;
using namespace siftwing::test;
unsigned int checks = 0;
std::atomic<bool> arm_job{false}, entered{false}, release_job{false};
std::atomic<unsigned int> executions{0}, accepted{0}, full{0}, preserved{0};
std::atomic<int> pin_next{-1}, watch_next{0}, slow_fd{-1};
std::atomic<bool> block_output{false};
std::atomic<std::size_t> sent{0}, read_bytes{0}, blocked{0};
std::atomic<unsigned int> interest{0};
std::mutex routes_mutex;
std::array<ConnectionId, 2> routes{};
std::size_t route_count = 0;

void expect(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void check(bool ok, const char *why) {
    ++checks; // 仅控制线程计数；客户端线程通过异常和原子进度汇报。
    expect(ok, why);
}
struct ReleaseGate {
    ~ReleaseGate() {
        release_job = true;
        block_output = false;
    }
};
struct Joined {
    std::vector<std::thread> threads;
    ~Joined() {
        for (auto &thread : threads)
            if (thread.joinable())
                thread.join();
    }
    void join() {
        for (auto &thread : threads)
            thread.join();
    }
};
}

query::SearchServiceResult real_execute(const query::SearchService *, const SearchTask &) __asm__(
    "__real__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskE");
query::SearchServiceResult
wrapped_execute(const query::SearchService *, const SearchTask &) __asm__(
    "__wrap__ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskE");
query::SearchServiceResult wrapped_execute(
    const query::SearchService *service, const SearchTask &task) {
    ++executions;
    if (arm_job.exchange(false)) {
        entered = true;
        while (!release_job.load())
            std::this_thread::yield();
    }
    return real_execute(service, task);
}
TaskSubmitResult real_submit(WorkerPool *, std::unique_ptr<WorkerTask> &) __asm__(
    "__real__ZN8siftwing11concurrency10WorkerPool10try_submitERSt10unique_ptrINS0_10WorkerTaskESt14default_deleteIS3_EE");
TaskSubmitResult wrapped_submit(WorkerPool *, std::unique_ptr<WorkerTask> &) __asm__(
    "__wrap__ZN8siftwing11concurrency10WorkerPool10try_submitERSt10unique_ptrINS0_10WorkerTaskESt14default_deleteIS3_EE");
TaskSubmitResult wrapped_submit(WorkerPool *pool, std::unique_ptr<WorkerTask> &source) {
    const auto *original = source.get();
    const auto result = real_submit(pool, source);
    if (result == TaskSubmitResult::accepted)
        ++accepted;
    if (result == TaskSubmitResult::full) {
        if (source.get() == original && original)
            ++preserved;
        ++full;
    }
    return result;
}
std::variant<RoutedSearchTask, RouteError> real_route(ConnectionId, SearchTask &) noexcept __asm__(
    "__real__ZN8siftwing11concurrency16RoutedSearchTask6createENS_3net12ConnectionIdERNS0_10SearchTaskE");
std::variant<RoutedSearchTask, RouteError>
wrapped_route(ConnectionId, SearchTask &) noexcept __asm__(
    "__wrap__ZN8siftwing11concurrency16RoutedSearchTask6createENS_3net12ConnectionIdERNS0_10SearchTaskE");
std::variant<RoutedSearchTask, RouteError> wrapped_route(
    ConnectionId route, SearchTask &source) noexcept {
    const bool watched = source.request() && source.request()->request_id == 11;
    auto result = real_route(route, source);
    if (watched && std::holds_alternative<RoutedSearchTask>(result)) {
        const std::lock_guard<std::mutex> lock(routes_mutex);
        if (route_count < routes.size())
            routes[route_count] = route;
        ++route_count;
    }
    return result;
}
extern "C" {
int __real_accept4(int, sockaddr *, socklen_t *, int);
int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags) {
    int result = __real_accept4(fd, address, size, flags);
    if (result < 0)
        return result;
    const int target = pin_next.exchange(-1);
    if (target >= 0 && target != result) {
        // 只接管已确认空闲的测试fd，绝不覆盖其他对象；仍使用真实accept的同一socket。
        if (::fcntl(target, F_GETFD) != -1 || errno != EBADF
            || ::dup3(result, target, O_CLOEXEC) != target) {
            (void)::close(result);
            errno = EBUSY;
            return -1;
        }
        (void)::close(result);
        result = target;
    }
    if (watch_next.exchange(0))
        slow_fd = result;
    return result;
}
ssize_t __real_send(int, const void *, std::size_t, int);
ssize_t __wrap_send(int fd, const void *data, std::size_t size, int flags) {
    if (fd != slow_fd.load())
        return __real_send(fd, data, size, flags);
    const auto prefix = sent.load();
    if (block_output.load()) {
        if (prefix >= 5) {
            ++blocked;
            errno = EAGAIN;
            return -1;
        }
        size = std::min(size, 5 - prefix);
    }
    const auto result = __real_send(fd, data, size, flags);
    if (result > 0)
        sent.fetch_add(static_cast<std::size_t>(result));
    return result;
}
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __wrap_recv(int fd, void *data, std::size_t size, int flags) {
    const auto result = __real_recv(fd, data, size, flags);
    if (fd == slow_fd.load() && result > 0)
        read_bytes.fetch_add(static_cast<std::size_t>(result));
    return result;
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int fd, int op, int target, epoll_event *event) {
    const auto result = __real_epoll_ctl(fd, op, target, event);
    if (result == 0 && target == slow_fd.load() && op != EPOLL_CTL_DEL)
        interest = event->events;
    return result;
}
}
namespace {
/** @brief 独立24字节参考验证完整响应；客户端线程不修改控制线程的检查计数。 */
std::string response(int fd, std::uint16_t type, std::uint64_t id, std::uint16_t status = 0) {
    const auto header = receive(fd, 24);
    std::size_t size = 0;
    for (std::size_t i = 12; i < 16; ++i)
        size = size * 256 + static_cast<unsigned char>(header[i]);
    expect(size <= 1024 * 1024, "bounded response length");
    const auto body = receive(fd, size);
    expect(
        header + body == wire(type, id, body, status), "independent response type/id/status/bytes");
    return body;
}
void no_more(int fd) {
    await([&] {
        char byte;
        const auto n = ::recv(fd, &byte, 1, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno == ECONNRESET))
            return true;
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return false;
        throw std::runtime_error("unexpected extra response or EOF error");
    });
}
void reset() {
    arm_job = false;
    entered = false;
    release_job = false;
    executions = 0;
    accepted = 0;
    full = 0;
    preserved = 0;
    pin_next = -1;
    watch_next = 0;
    slow_fd = -1;
    block_output = false;
    sent = 0;
    read_bytes = 0;
    blocked = 0;
    interest = 0;
    const std::lock_guard<std::mutex> lock(routes_mutex);
    routes = {};
    route_count = 0;
}
std::unique_ptr<OnlineServer> create(const std::string &parent, std::size_t subs, bool reuse) {
    query::SearchServiceConfig resources;
    resources.parent = parent;
    auto loaded = query::SearchService::create(resources);
    if (!loaded)
        throw std::runtime_error(loaded.error().context);
    OnlineServerConfig config;
    config.sub_count = subs;
    config.workers = reuse ? 1 : 3;
    config.task_capacity = reuse ? 1 : 64;
    config.sub.read_byte_budget = 9;
    config.sub.write_byte_budget = 7;
    config.sub.io_attempt_budget = 2;
    auto built = OnlineServer::create(std::move(loaded).value(), config);
    if (const auto *error = std::get_if<std::error_code>(&built))
        throw std::system_error(*error);
    return std::move(std::get<std::unique_ptr<OnlineServer>>(built));
}
UniqueFd attach(OnlineServer &server, std::size_t &connected) {
    auto peer = client(server.port());
    ++connected;
    await([&] {
        if (const auto error = server.run_once(0))
            throw std::system_error(error);
        return server.report().handed_off == connected;
    });
    return peer;
}
std::size_t stale(const OnlineServerReport &report) {
    std::size_t count = 0;
    for (const auto &sub : report.subs)
        count += sub.completion_stale;
    return count;
}
void final_accounts(const OnlineServerReport &report, std::size_t jobs) {
    check(report.stopped && !report.first_error && report.workers.succeeded == jobs
              && report.workers.failed == 0,
        "all accepted real jobs finish before service teardown");
    for (const auto &sub : report.subs) {
        check(sub.state == ProtocolSubState::joined && sub.active == 0
                  && sub.admitted == sub.dropped && !sub.first_error && !sub.cleanup_error,
            "joined Sub owns no connections and local faults do not fail service");
        check(sub.completion_received
                  == sub.completion_applied + sub.completion_rejected + sub.completion_stale,
            "every received completion has exactly one outcome");
    }
}
/** @brief 同fd、同Sub/slot、同关联号且新任务仍pending时交付旧结果，排除仅凭fd或id路由。 */
void reuse(const std::string &parent, std::size_t subs) {
    reset();
    arm_job = true;
    const auto baseline = fd_count();
    {
        auto server = create(parent, subs, true);
        ReleaseGate release_on_exit;
        std::size_t connected = 0;
        UniqueFd reservation(::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 128));
        check(static_cast<bool>(reservation),
            "reserve unused high descriptor without fixed-number clobber");
        const int reused_fd = reservation.get();
        check(!reservation.close(), "make reserved descriptor available for real accepted socket");
        pin_next = reused_fd;
        auto old = attach(*server, connected);
        send_all(old.get(), wire(3, 11, R"({"query":"unknown"})"));
        await([] {
            return entered.load() && accepted.load() == 1;
        });
        check(::fcntl(reused_fd, F_GETFD) >= 0, "original accepted fd is real and live");
        const linger reset_peer{1, 0};
        check(::setsockopt(old.get(), SOL_SOCKET, SO_LINGER, &reset_peer, sizeof(reset_peer)) == 0
                  && !old.close(),
            "real RST while original worker is held");
        await([&] {
            return server->report().subs[0].dropped == 1 && ::fcntl(reused_fd, F_GETFD) == -1
                   && errno == EBADF;
        });
        UniqueFd other_sub;
        if (subs == 2)
            other_sub = attach(*server, connected); // 保持Main轮转，替换源回到原Sub。
        pin_next = reused_fd;
        auto replacement = attach(*server, connected);
        send_all(replacement.get(), wire(3, 11, R"({"query":"red"})") + wire(1, 33));
        check(::shutdown(replacement.get(), SHUT_WR) == 0,
            "replacement keeps cached PING through half close");
        await([] {
            return accepted.load() == 2;
        });
        {
            const std::lock_guard<std::mutex> lock(routes_mutex);
            check(route_count == 2 && routes[0].reactor_id == routes[1].reactor_id
                      && routes[0].slot_index == routes[1].slot_index
                      && routes[1].generation == routes[0].generation + 1,
                "same slot/reactor/request id is distinguished by next generation");
        }
        check(::fcntl(reused_fd, F_GETFD) >= 0 && pin_next.load() == -1,
            "replacement is using the original real descriptor number");
        auto rejected = attach(*server, connected);
        send_all(rejected.get(), wire(4, 12, R"({"key":"cat"})") + wire(1, 34));
        check(response(rejected.get(), 0xffff, 12, 2) == R"({"message":"overloaded"})"
                  && response(rejected.get(), 0x8001, 34) == "{}",
            "real full queue rejects without blocking PING");
        check(full.load() == 1 && preserved.load() == 1 && executions.load() == 1
                  && !release_job.load(),
            "full task source retained; replacement has not executed");
        release_job = true;
        const auto answer = nlohmann::json::parse(response(replacement.get(), 0x8003, 11));
        check(!answer.at("results").empty(),
            "new same-id SEARCH gets red result, never old empty result");
        check(response(replacement.get(), 0x8001, 33) == "{}",
            "cached frame follows exactly one new business response");
        no_more(replacement.get());
        await([&] {
            return stale(server->report()) == 1;
        });
        check(executions.load() == 2 && accepted.load() == 2 && full.load() == 1,
            "old disconnected and new queued task each execute once; rejected task never runs");
        check(!server->shutdown(1000), "graceful close after reuse recovery");
        const auto report = server->report();
        check(stale(report) == 1 && report.subs[0].completion_applied == 1,
            "old result is stale while replacement is live, never applied or retried");
        final_accounts(report, 2);
    }
    check(fd_count() == baseline, "reuse cycle returns every descriptor to baseline");
}
/** @brief 受控慢输出与两种半包持续存在时，四客户端并发执行四命令和缓存帧。 */
void mixed(const std::string &parent, std::size_t subs) {
    reset();
    const auto baseline = fd_count();
    {
        auto server = create(parent, subs, false);
        ReleaseGate release_on_exit;
        std::size_t connected = 0;
        watch_next = 1;
        block_output = true;
        auto slow = attach(*server, connected);
        send_all(slow.get(), wire(1, 70) + wire(1, 71));
        await([] {
            return blocked.load() > 0 && sent.load() == 5 && (interest.load() & EPOLLOUT)
                   && !(interest.load() & EPOLLIN);
        });
        const auto before_reads = read_bytes.load();
        auto partial_header = attach(*server, connected);
        auto partial_body = attach(*server, connected);
        const auto first = wire(1, 72), second = wire(1, 73);
        send_all(partial_header.get(), first.substr(0, 13));
        send_all(partial_body.get(), second.substr(0, 25));
        auto malformed = attach(*server, connected);
        auto bad = wire(1, 74);
        bad.replace(0, 4, "BAD!");
        send_all(malformed.get(), bad);
        no_more(malformed.get());
        std::array<UniqueFd, 4> peers;
        for (auto &peer : peers)
            peer = attach(*server, connected);
        std::array<std::exception_ptr, 4> failures{};
        std::atomic<unsigned int> done{0}, completed{0};
        Joined clients;
        for (std::size_t index = 0; index < peers.size(); ++index) {
            clients.threads.emplace_back([&, index] {
                try {
                    const int fd = peers[index].get();
                    for (std::uint64_t round = 0; round < 12; ++round) {
                        const std::uint64_t id = 1000 + 100 * index + 2 * round;
                        const auto type = static_cast<std::uint16_t>(1 + round % 4);
                        const std::string body = type == 3   ? R"({"query":"unknown"})"
                                                 : type == 4 ? R"({"key":"cat"})"
                                                             : "{}";
                        const auto packet = wire(type, id, body) + wire(1, id + 1);
                        // 显式分两次send；不能声称TCP必然按该边界分段或recv。
                        send_all(fd, std::string_view(packet).substr(0, 13));
                        send_all(fd, std::string_view(packet).substr(13));
                        const auto value = nlohmann::json::parse(
                            response(fd, static_cast<std::uint16_t>(0x8000U + type), id));
                        if (type == 1)
                            expect(value.empty(), "PING body");
                        if (type == 2)
                            expect(value.at("command") == "SERVER_INFO"
                                       && value.at("snapshot_version") == "0",
                                "fixed server information");
                        if (type == 3)
                            expect(value.at("results").empty(), "real no-match query");
                        if (type == 4)
                            expect(
                                value.at("results").at(0).at("term") == "cat", "real suggestion");
                        expect(response(fd, 0x8001, id + 1) == "{}", "cached PING remains ordered");
                        completed.fetch_add(2);
                    }
                    expect(::shutdown(fd, SHUT_WR) == 0, "parallel client half close");
                    no_more(fd);
                } catch (...) {
                    failures[index] = std::current_exception();
                }
                ++done;
            });
        }
        await([&] {
            if (const auto error = server->run_once(0))
                throw std::system_error(error);
            const auto report = server->report();
            for (const auto &sub : report.subs)
                expect(sub.active <= 128 && sub.responses <= sub.requests && !sub.first_error,
                    "live report invariants during concurrent I/O");
            return done.load() == peers.size();
        });
        clients.join();
        for (const auto &failure : failures)
            if (failure)
                std::rethrow_exception(failure);
        check(completed.load() == 96,
            "four clients validate 96 complete independently framed responses");
        check(block_output.load() && sent.load() == 5 && read_bytes.load() == before_reads,
            "healthy business completes while slow pending connection reads no next request");
        check(executions.load() == 24 && accepted.load() == 24 && full.load() == 0,
            "all parallel SEARCH/SUGGEST jobs execute exactly once");
        send_all(partial_header.get(), std::string_view(first).substr(13));
        send_all(partial_body.get(), std::string_view(second).substr(25));
        check(response(partial_header.get(), 0x8001, 72) == "{}"
                  && response(partial_body.get(), 0x8001, 73) == "{}",
            "held header/body resume on their own connections");
        block_output = false;
        check(response(slow.get(), 0x8001, 70) == "{}" && response(slow.get(), 0x8001, 71) == "{}",
            "partial real output resumes and cached request follows without retransmit");
        check(!server->shutdown(1000), "graceful close after mixed traffic");
        const auto report = server->report();
        check(
            report.accepted == connected && report.handed_off == connected && report.rejected == 0,
            "every mixed client handed off once");
        check(stale(report) == 0, "healthy parallel jobs have no stray identities");
        final_accounts(report, 24);
        slow_fd = -1; // 所属线程join后才撤测试借用；不会把复用后的客户端fd误当旧观察端。
    }
    check(fd_count() == baseline, "mixed cycle returns every descriptor to baseline");
}
}
int main(int argc, char **argv) {
    try {
        if (argc != 3 || (std::string(argv[2]) != "1" && std::string(argv[2]) != "2"))
            throw std::runtime_error("snapshot-parent and sub-count 1/2 required");
        const std::size_t subs = std::string(argv[2]) == "1" ? 1U : 2U;
        reuse(argv[1], subs);
        mixed(argv[1], subs);
        std::cout << checks << " V2 fault/concurrency checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << "V2 fault/concurrency failed: " << error.what() << '\n';
        return 1;
    }
}
