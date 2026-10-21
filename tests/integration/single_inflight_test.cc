/*
 * PROJECT : SIFTWING
 * FILE    : single_inflight_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 单连接等待worker、部分输出与错误响应期间的确定门禁证明
 * IMPLEMENTATION : 真实连接核心/WorkerPool，静态系统调用观察和独立黄金线字节。
 */
#include "../support/completion_fixture.hpp"
#include <sys/epoll.h>

namespace {
using namespace siftwing::test;
unsigned int checks = 0;
std::atomic<int> watched{-1};
std::atomic<unsigned int> read_calls{0}, interest{0}, blocked_writes{0};
std::atomic<std::size_t> written{0};
std::atomic<std::size_t> received_bytes{0};
std::atomic<bool> hold_output{false};
constexpr std::size_t prefix_bytes = 7;
void check(bool value, const char *why) {
    require(value, why, checks);
}
}
extern "C" ssize_t __real_recv(int, void *, size_t, int);
extern "C" ssize_t __wrap_recv(int fd, void *bytes, size_t size, int flags) {
    if (fd == watched.load())
        ++read_calls;
    const auto result = __real_recv(fd, bytes, size, flags);
    if (fd == watched.load() && result > 0)
        received_bytes.fetch_add(static_cast<std::size_t>(result));
    return result;
}
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
extern "C" int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event) {
    const int result = __real_epoll_ctl(epoll, op, fd, event);
    if (result == 0 && fd == watched.load() && op != EPOLL_CTL_DEL)
        interest = event->events;
    return result;
}
extern "C" ssize_t __real_send(int, const void *, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void *bytes, size_t size, int flags) {
    if (fd != watched.load())
        return __real_send(fd, bytes, size, flags);
    // 先真实发送精确前缀，随后确定EAGAIN；不靠socket容量或调度时间猜测短写。
    if (hold_output.load()) {
        const auto sent = written.load();
        if (sent >= prefix_bytes) {
            ++blocked_writes;
            errno = EAGAIN;
            return -1;
        }
        size = std::min(size, prefix_bytes - sent);
    }
    const auto result = __real_send(fd, bytes, size, flags);
    if (result > 0)
        written.fetch_add(static_cast<std::size_t>(result));
    return result;
}

namespace {
/** @brief 观测fd在移交前设置，直到连接核心 join才撤销；peer仍由控制者独占。 */
UniqueFd attach_watched(ConnectionHarness &sub, std::string_view initial = {}) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair))
        throw std::system_error(errno, std::generic_category());
    UniqueFd source(pair[0]), peer(pair[1]);
    watched = source.get();
    read_calls = 0;
    interest = 0;
    written = 0;
    blocked_writes = 0;
    received_bytes = 0;
    if (!initial.empty())
        send_all(peer.get(), initial);
    check(!sub.adopt(source), "watched stream handed to real 连接核心");
    return peer;
}
void eof(int fd) {
    await([&] {
        char byte;
        const auto n = ::recv(fd, &byte, 1, MSG_DONTWAIT);
        if (n != 0 && !(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)))
            throw std::runtime_error("unexpected byte or recv error before EOF");
        return n == 0;
    });
    check(true, "EOF follows all complete responses without extra byte");
}
/** @brief 另一连接的PING往返证明同一连接核心仍在服务；与被观察流的业务进度无关。 */
void checkpoint(int fd, std::uint64_t id) {
    send_all(fd, wire(1, id));
    const auto expected = wire(0x8001, id);
    check(receive(fd, expected.size()) == expected, "other connection progresses on same 连接核心");
}
/** @brief 真实worker持有唯一任务；控制者释放前不能构造或发布完成。 */
class PausedJob final : public WorkerTask {
public:
    PausedJob(std::unique_ptr<RoutedSearchTask> task,
        CompletionTarget target,
        std::atomic<bool> &entered,
        std::atomic<bool> &release)
        : task_(std::move(task)), target_(target), entered_(entered), release_(release) {}
    void execute() override {
        entered_ = true;
        await([&] {
            return release_.load();
        });
        publish(target_, completion(*task_, "{\"worker\":1}"));
    }

private:
    std::unique_ptr<RoutedSearchTask> task_;
    CompletionTarget target_;
    std::atomic<bool> &entered_, &release_;
};
void worker_gate() {
    std::atomic<bool> entered{false}, release{false}; // 生命覆盖pool析构中的失败清理join。
    CaptureSubmitter sink;
    ProtocolConnectionConfig config;
    config.request_submitter = &sink;
    config.reactor_id = 25;
    ConnectionHarness sub(config);
    WorkerPool pool(1, 2);
    check(!sub.start(), "worker gate 连接核心 ready");
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    auto peer = attach_watched(sub);
    auto control = attach(sub);
    send_all(peer.get(), wire(3, 77));
    auto first_task = take_request(sink);
    const auto route = first_task->route();
    std::unique_ptr<WorkerTask> job =
        std::make_unique<PausedJob>(std::move(first_task), target, entered, release);
    check(
        pool.try_submit(job) == TaskSubmitResult::accepted && !job, "real worker owns first task");
    await([&] {
        return entered.load() && interest.load() == 0;
    });
    const auto reads = read_calls.load();
    // 后续帧留在内核；FIN也不能使等待中的完整业务请求被丢弃。
    send_all(peer.get(), wire(4, 77) + wire(1, UINT64_MAX));
    check(::shutdown(peer.get(), SHUT_WR) == 0, "half close while real worker is paused");
    for (std::uint64_t n = 1; n <= 8; ++n)
        checkpoint(control.get(), n);
    check(read_calls.load() == reads && !sink.requests.try_pop(),
        "waiting worker performs no recv or second submission");
    // 在途上限属于连接；同连接核心另一连接仍可向提交器交付业务。
    send_all(control.get(), wire(3, 77));
    auto independent = take_request(sink);
    check(independent->route() != route, "other connection business is independently admitted");
    publish(target, completion(*independent, "{\"other\":1}"));
    const auto other = wire(0x8003, 77, "{\"other\":1}");
    check(
        receive(control.get(), other.size()) == other, "other connection response before release");
    release = true;
    const auto first = wire(0x8003, 77, "{\"worker\":1}");
    check(receive(peer.get(), first.size()) == first, "worker result survives peer half close");
    auto second = take_request(sink);
    check(second->route() == route && second->payload()->request()->request_id == 77
              && second->payload()->request()->message_type == MessageType::suggest_request,
        "same request id may be reused after prior output drains");
    publish(target, completion(*second, "{\"next\":2}"));
    const auto tail = wire(0x8004, 77, "{\"next\":2}") + wire(0x8001, UINT64_MAX);
    check(receive(peer.get(), tail.size()) == tail, "kernel tail and PING remain ordered");
    eof(peer.get());
    pool.join(); // 覆盖完整publish/notify；随后才允许连接核心撤目标和close。
    check(pool.report().succeeded == 1 && pool.report().failed == 0,
        "real worker completed exactly once");
    stop_core(sub);
    watched = -1;
}
void output_gate() {
    CaptureSubmitter sink;
    ProtocolConnectionConfig config;
    config.request_submitter = &sink;
    config.max_body_bytes = 65536;
    config.write_byte_budget = 5;
    config.io_attempt_budget = 2;
    ConnectionHarness sub(config);
    check(!sub.start(), "output gate 连接核心 ready");
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    hold_output = true;
    const auto packet = wire(3, 77) + wire(4, 77) + wire(1, 78);
    auto peer = attach_watched(sub, packet);
    auto control = attach(sub);
    // 一次recv可同时缓存三帧；后续帧字节存在不等于已经交付业务。
    auto task = take_request(sink);
    await([&] {
        return interest.load() == 0;
    });
    check(received_bytes.load() == packet.size(),
        "all three frames really reside in Connection input");
    const auto reads = read_calls.load();
    const std::string body = "{\"pad\":\"" + std::string(32768, 'x') + "\"}";
    const auto expected = wire(0x8003, 77, body);
    publish(target, completion(*task, body));
    await([&] {
        return blocked_writes.load() != 0 && interest.load() == EPOLLOUT;
    });
    check(receive(peer.get(), prefix_bytes) == expected.substr(0, prefix_bytes),
        "actual partial wire prefix");
    // 当前响应已复制，但未排空；错ID/类型/重复完成均不能解除门禁。
    publish(target, completion(*fabricated_task(task->route(), 99)));
    publish(target, completion(*fabricated_task(task->route(), 77, MessageType::suggest_request)));
    publish(target, completion(*task));
    await([&] {
        return sub.status().completion_rejected == 3;
    });
    for (std::uint64_t n = 1; n <= 8; ++n)
        checkpoint(control.get(), n);
    await([&] {
        return sub.status().responses == 8;
    });
    check(read_calls.load() == reads && !sink.requests.try_pop(),
        "partial output gates cached next request");
    check(
        sub.status().requests == 9 && sub.status().responses == 8 && written.load() == prefix_bytes,
        "only control PINGs finish while watched response is blocked");
    hold_output = false;
    check(receive(peer.get(), expected.size() - prefix_bytes) == expected.substr(prefix_bytes),
        "release preserves exact unsent suffix");
    auto next = take_request(sink);
    check(next->payload()->request()->message_type == MessageType::suggest_request,
        "cached next frame is actively resumed without a new peer send");
    publish(target, completion(*next, "{\"ok\":true}"));
    const auto tail = wire(0x8004, 77, "{\"ok\":true}") + wire(0x8001, 78);
    check(receive(peer.get(), tail.size()) == tail, "buffered order after drain");
    check(::shutdown(peer.get(), SHUT_WR) == 0, "half close after output gate");
    eof(peer.get());
    stop_core(sub);
    watched = -1;
}
void error_gates() {
    for (unsigned int scenario = 0; scenario < 4; ++scenario) {
        CaptureSubmitter sink;
        if (scenario == 1)
            sink.refusal = TaskSubmitResult::full;
        if (scenario == 2)
            sink.refusal = TaskSubmitResult::closed;
        ProtocolConnectionConfig config;
        config.request_submitter = &sink;
        ConnectionHarness sub(config);
        check(!sub.start(), "error gate 连接核心 ready");
        const auto target = std::get<CompletionTarget>(sub.completion_target());
        hold_output = true;
        auto peer = attach_watched(sub);
        auto control = attach(sub);
        send_all(peer.get(), wire(3, 77, scenario == 0 ? "[]" : "{}") + wire(1, 77) + wire(4, 77));
        check(::shutdown(peer.get(), SHUT_WR) == 0,
            "half close behind error and cached valid frames");
        // 第四种为任务成功接纳后返回的业务错误；前三种在I/O线程直接生成错误响应。
        if (scenario == 3) {
            auto task = take_request(sink);
            auto error = RoutedCompletion::error(*task, ErrorStatus::invalid_request);
            publish(target,
                std::make_unique<RoutedCompletion>(std::get<RoutedCompletion>(std::move(error))));
        }
        await([&] {
            return blocked_writes.load() != 0 && interest.load() == EPOLLOUT;
        });
        const auto reads = read_calls.load();
        checkpoint(control.get(), 100);
        await([&] {
            return sub.status().responses == 1;
        });
        check(sub.status().requests == 2 && sub.status().responses == 1 && !sink.requests.try_pop()
                  && read_calls.load() == reads,
            "error response retains gate until actual drain");
        const std::uint16_t status = scenario == 1 ? 2 : scenario == 2 ? 3 : 1;
        const auto message = scenario == 1   ? "{\"message\":\"overloaded\"}"
                             : scenario == 2 ? "{\"message\":\"stopping\"}"
                                             : "{\"message\":\"invalid request\"}";
        const auto first = wire(0xffff, 77, message, status);
        hold_output = false;
        const auto head = first + wire(0x8001, 77);
        check(receive(peer.get(), head.size()) == head, "error then same-id PING in exact order");
        if (scenario == 1 || scenario == 2) {
            check(receive(peer.get(), first.size()) == first,
                "next business refusal keeps safe status");
        } else {
            auto next = take_request(sink);
            publish(target, completion(*next, "{\"recovered\":true}"));
            const auto response = wire(0x8004, 77, "{\"recovered\":true}");
            check(receive(peer.get(), response.size()) == response,
                "valid request continues after error drain");
        }
        eof(peer.get());
        stop_core(sub);
        watched = -1;
    }
}
} // namespace
int main(int argc, char **argv) {
    const auto baseline = fd_count();
    if (argc != 2)
        return 2;
    const std::string_view suite(argv[1]);
    if (suite == "worker")
        worker_gate();
    else if (suite == "output")
        output_gate();
    else if (suite == "errors")
        error_gates();
    else
        return 2;
    check(
        fd_count() == baseline, "all watched connections, worker and 连接核心 resources reclaimed");
    std::cout << "single inflight " << suite << " checks=" << checks << " failures=0\n";
}
