/*
 * PROJECT : SIFTWING
 * FILE    : output_backpressure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 精确水位、原子硬限拒绝、所属Sub局部策略与真实TCP慢读隔离
 * IMPLEMENTATION : 独立黄金字节，真实前缀send配合确定EAGAIN；TCP专项不注入发送结果。
 */
#include "../support/completion_fixture.hpp"
#include <sys/epoll.h>

namespace {
using namespace siftwing::test;
unsigned int checks = 0;
std::atomic<int> watched{-1};
std::atomic<std::size_t> sent_bytes{0}, recv_bytes{0}, read_calls{0};
std::atomic<std::size_t> send_ceiling{SIZE_MAX};
std::atomic<unsigned int> interest{0}, blocked{0};
std::atomic<bool> watch_accept{false};
void check(bool value, const char *why) {
    require(value, why, checks);
}
void observe(int fd) {
    watched = fd;
    sent_bytes = 0;
    recv_bytes = 0;
    read_calls = 0;
    interest = 0;
    blocked = 0;
}
std::string body(std::size_t bytes) {
    return "{\"p\":\"" + std::string(bytes - 8, 'x') + "\"}";
}
void eof(int fd) {
    bool ended = false;
    await([&] {
        char byte = 0;
        const auto n = ::recv(fd, &byte, 1, MSG_DONTWAIT);
        if (n > 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            throw std::runtime_error("unexpected trailing byte or socket failure");
        ended = n == 0;
        return ended;
    });
    check(ended, "peer sees EOF after complete ordered output or local close");
}
void ping(int fd, std::uint64_t id) {
    send_all(fd, wire(1, id));
    check(receive(fd, 26) == wire(0x8001, id), "other connection continues PING");
}
UniqueFd attached(ProtocolSubReactor &sub, std::string_view packet) {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds))
        throw std::system_error(errno, std::generic_category());
    UniqueFd source(fds[0]), peer(fds[1]);
    observe(source.get());
    send_all(peer.get(), packet);
    const auto target = std::get<MainReactorTarget>(sub.target());
    const auto result = try_handoff_and_notify(*target.queue, source, target.wakeup_fd);
    check(result.handoff == FdHandoffResult::accepted && !result.notification_error,
        "watched source handed to real Sub");
    return peer;
}
}
extern "C" {
ssize_t __real_send(int, const void *, std::size_t, int);
ssize_t __wrap_send(int fd, const void *bytes, std::size_t size, int flags) {
    if (fd != watched.load())
        return __real_send(fd, bytes, size, flags);
    const auto sent = sent_bytes.load(), ceiling = send_ceiling.load();
    if (sent >= ceiling) {
        ++blocked;
        errno = EAGAIN;
        return -1;
    }
    const auto n = __real_send(fd, bytes, std::min(size, ceiling - sent), flags);
    if (n > 0)
        sent_bytes.fetch_add(static_cast<std::size_t>(n));
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        ++blocked;
    return n;
}
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __wrap_recv(int fd, void *bytes, std::size_t size, int flags) {
    const auto n = __real_recv(fd, bytes, size, flags);
    if (fd == watched.load()) {
        ++read_calls;
        if (n > 0)
            recv_bytes.fetch_add(static_cast<std::size_t>(n));
    }
    return n;
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int fd, int op, int target, epoll_event *event) {
    const int result = __real_epoll_ctl(fd, op, target, event);
    if (result == 0 && target == watched.load() && op != EPOLL_CTL_DEL)
        interest = event->events;
    return result;
}
int __real_accept4(int, sockaddr *, socklen_t *, int);
int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags) {
    const int accepted = __real_accept4(fd, address, size, flags);
    if (accepted >= 0 && watch_accept.exchange(false)) {
        const int capacity = 4096;
        if (::setsockopt(accepted, SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)))
            std::abort();
        observe(accepted); // Main发布之前冻结观察fd，join后才撤销。
    }
    return accepted;
}
}
namespace {
void limits() {
    const auto defaults = checked_output_watermarks(128);
    check(defaults.hard_bytes == 152 && defaults.high_bytes == 114 && defaults.low_bytes == 76,
        "automatic watermarks preserve prior per-frame hard capacity");
    const std::array<OutputWatermarks, 7> invalid{{{0, 0, 96},
        {32, 32, 96},
        {65, 64, 96},
        {0, 97, 96},
        {0, 1, 25},
        {0, 1, 537},
        {SIZE_MAX, 1, 96}}};
    for (const auto value : invalid) {
        int fds[2];
        check(
            ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0, "invalid config pair");
        UniqueFd source(fds[0]), peer(fds[1]);
        bool rejected = false;
        try {
            Connection connection(std::move(source), 512, 0, value);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        check(rejected && source.get() == fds[0] && ::fcntl(source.get(), F_GETFD) >= 0,
            "invalid output config leaves source fd untouched");
        ProtocolSubConfig config;
        config.max_body_bytes = 512;
        config.output = value;
        rejected = false;
        try {
            ProtocolSubReactor sub(config);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        check(rejected, "Sub rejects same invalid config before start");
    }
    bool rejected = false;
    try {
        (void)checked_output_watermarks(SIZE_MAX);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "body limit validated before size addition");
    check(checked_output_watermarks(2, {0, 1, 26}).hard_bytes == 26,
        "zero low and minimum PONG capacity allowed");
    // high-1/high/high+1及hard/hard+1独立构造，每个响应的线大小由黄金24字节头计算。
    for (const std::size_t total : {63U, 64U, 65U, 96U, 97U}) {
        int fds[2];
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0, "limit pair");
        UniqueFd source(fds[0]), peer(fds[1]);
        Connection connection(std::move(source), 512, 0, {32, 64, 96});
        send_all(peer.get(), wire(3, 77) + wire(1, 78));
        check(connection.read_request().stop == ReadStop::request, "trusted request pending");
        const Frame response{MessageType::search_response, 0, 77, body(total - 24)};
        const auto copy = response.body;
        const auto queued = connection.queue_response(response);
        check(response.body == copy && connection.request_pending(),
            "queue preserves source and pending");
        if (total > 96) {
            check(queued.stop == QueueStop::output_limit && connection.output_bytes() == 0
                      && !connection.output_backpressured(),
                "hard+1 atomic refusal");
            auto wrong = response;
            wrong.request_id = 99;
            check(connection.queue_response(wrong).stop == QueueStop::request_mismatch,
                "mismatch precedes capacity and cannot replace current request");
            wrong = response;
            wrong.body[0] = '[';
            check(connection.queue_response(wrong).stop == QueueStop::protocol_error,
                "invalid JSON precedes output limit");
            const auto small = std::get<Frame>(make_error_response(
                Frame{MessageType::search_request, 0, 77, "{}"}, ErrorStatus::resource_limit, 512));
            check(connection.queue_response(small).stop == QueueStop::queued,
                "refusal allows a smaller same-request error");
            check(connection.flush_output().stop == WriteStop::drained, "small fallback drains");
            check(receive(peer.get(), small.body.size() + 24) == wire(0xffff, 77, small.body, 6),
                "no bytes from refused response escape");
        } else {
            check(queued.stop == QueueStop::queued && connection.output_bytes() == total
                      && connection.output_backpressured() == (total >= 64),
                "exact high and hard boundary");
            if (total == 96) {
                check(connection.flush_output(0, 1).stop == WriteStop::invalid_budget
                          && connection.output_backpressured(),
                    "invalid budget preserves high state");
                check(connection.flush_output(33).bytes == 33 && connection.output_bytes() == 63
                          && connection.output_backpressured(),
                    "below high remains paused");
                check(connection.flush_output(30).bytes == 30 && connection.output_bytes() == 33
                          && connection.output_backpressured(),
                    "low+1 remains paused");
                check(connection.flush_output(1).bytes == 1 && connection.output_bytes() == 32
                          && !connection.output_backpressured(),
                    "exact low clears output pause");
                const auto read = connection.read_request();
                check(read.stop == ReadStop::request_pending && read.attempts == 0
                          && !connection.finish_request(),
                    "low does not admit cached next request");
            }
            check(connection.flush_output().stop == WriteStop::drained
                      && !connection.output_backpressured(),
                "remaining bytes drain and remove write pressure");
            check(receive(peer.get(), total) == wire(0x8003, 77, response.body),
                "exact original bytes");
        }
        check(connection.finish_request() && connection.read_request().request->request_id == 78,
            "only drain allows cached request continuation");
        check(
            !connection.close() && !connection.output_backpressured(), "close clears water state");
    }
    // 高位关闭和真实EPIPE都必须清除滞回状态，不能在已终止对象残留读暂停原因。
    for (const bool io_failure : {false, true}) {
        int fds[2];
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0, "high close pair");
        UniqueFd source(fds[0]), peer(fds[1]);
        Connection connection(std::move(source), 512, 0, {32, 64, 96});
        send_all(peer.get(), wire(3, 77));
        check(connection.read_request().stop == ReadStop::request, "high close request");
        check(connection.queue_response(Frame{MessageType::search_response, 0, 77, body(40)}).stop
                      == QueueStop::queued
                  && connection.output_backpressured(),
            "high close output");
        if (io_failure) {
            check(::shutdown(connection.get(), SHUT_WR) == 0, "force real send failure");
            const auto write = connection.flush_output();
            check(write.stop == WriteStop::io_error && write.error.value() == EPIPE,
                "MSG_NOSIGNAL preserves EPIPE diagnostic");
        } else {
            check(!connection.close(), "explicit close from high water");
        }
        check(connection.get() == -1 && connection.output_bytes() == 0
                  && !connection.output_backpressured() && !connection.request_pending(),
            "termination clears output, hysteresis and pending together");
    }
}
void sub_policy() {
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.max_body_bytes = 512;
    config.output = {16, 48, 96};
    config.request_submitter = &sink;
    config.write_byte_budget = 5;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "policy Sub ready");
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    send_ceiling = 7;
    const auto packet = wire(3, 77) + wire(4, 77) + wire(1, 78);
    auto peer = attached(sub, packet);
    auto control = attach(sub);
    check(::shutdown(peer.get(), SHUT_WR) == 0, "FIN behind cached requests");
    auto task = take_request(sink);
    check(recv_bytes.load() == packet.size(), "all next frames actually cached");
    // 错关联的大响应先由原门禁拒绝，不能因超限替换正确任务。
    publish(target, completion(*fabricated_task(task->route(), 99), body(73)));
    await([&] {
        return sub.status().completion_rejected == 1;
    });
    check(sub.status().output_limit_rejections == 0, "wrong id is not output overflow");
    publish(target, completion(*task, body(73))); // 24+73=hard+1。
    const std::string error = "{\"message\":\"resource limit\"}";
    const auto expected = wire(0xffff, 77, error, 6);
    await([&] {
        return blocked.load() != 0 && interest.load() == EPOLLOUT;
    });
    check(receive(peer.get(), 7) == expected.substr(0, 7), "real prefix of fixed safe error");
    auto reads = read_calls.load();
    ping(control.get(), 100);
    check(read_calls.load() == reads && !sink.requests.try_pop(),
        "partial fallback gates next frame");
    // 降到低位但保留后缀；另一连接完成一次往返提供确定进度检查点。
    send_ceiling = expected.size() - 16;
    await([&] {
        return sent_bytes.load() == expected.size() - 16;
    });
    ping(control.get(), 101);
    check(read_calls.load() == reads && !sink.requests.try_pop(),
        "low water still respects in-flight gate");
    send_ceiling = SIZE_MAX;
    check(receive(peer.get(), expected.size() - 7) == expected.substr(7), "exact fallback suffix");
    auto next = take_request(sink);
    check(next->payload()->request()->message_type == MessageType::suggest_request,
        "same id cached next request actively resumed");
    publish(target, completion(*next, body(72))); // 恰好hard，允许完整发送。
    check(receive(peer.get(), 122) == wire(0x8004, 77, body(72)) + wire(0x8001, 78),
        "hard boundary then PING ordered");
    eof(peer.get());
    stop_sub(sub);
    watched = -1;
    const auto status = sub.status();
    check(status.output_limit_rejections == 1 && status.output_limit_closes == 0
              && status.completion_applied == 2 && !status.first_error,
        "local refusal accounted without Sub failure");
    // hard=26只容纳PONG，固定ERROR也不适配：异步结果、Body拒绝、full、closed分别局部关闭。
    for (unsigned int scenario = 0; scenario < 4; ++scenario) {
        CaptureSubmitter rejected_sink;
        if (scenario == 2)
            rejected_sink.refusal = TaskSubmitResult::full;
        if (scenario == 3)
            rejected_sink.refusal = TaskSubmitResult::closed;
        config.output = {0, 20, 26};
        config.request_submitter = &rejected_sink;
        ProtocolSubReactor small(config);
        check(!small.start(), "small hard Sub ready");
        auto limited = attach(small);
        auto healthy = attach(small);
        send_all(limited.get(), wire(3, 91, scenario == 1 ? "[]" : "{}"));
        if (scenario == 0) {
            auto owned = take_request(rejected_sink);
            publish(std::get<CompletionTarget>(small.completion_target()),
                completion(*owned, body(72)));
        }
        eof(limited.get());
        ping(healthy.get(), 92);
        await([&] {
            return small.status().responses == 1;
        });
        const auto report = small.status();
        check(report.output_limit_rejections == 1 && report.output_limit_closes == 1
                  && report.first_connection_error.value() == EMSGSIZE && !report.first_error
                  && report.active == 1,
            "only offending connection closed, PONG still fits");
        stop_sub(small);
    }
}
void tcp_slow_reader() {
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.max_body_bytes = 300000;
    config.write_byte_budget = 280000; // 大于本响应，避免先耗尽预算而根本没有机会观察真实EAGAIN。
    config.output = {32768, 65536, 280000};
    config.request_submitter = &sink;
    MultiReactorFixture fixture(1, config);
    check(!fixture.start(), "real TCP Main/Sub ready");
    watch_accept = true;
    send_ceiling = SIZE_MAX;
    auto peer = client(fixture.port());
    const int receive_capacity = 16384;
    check(
        ::setsockopt(peer.get(), SOL_SOCKET, SO_RCVBUF, &receive_capacity, sizeof(receive_capacity))
            == 0,
        "real bounded TCP receive buffer");
    send_all(peer.get(), wire(3, 77) + wire(1, 78));
    check(::shutdown(peer.get(), SHUT_WR) == 0, "TCP FIN behind two requests");
    auto task = take_request(sink);
    const auto payload = body(262144);
    const auto expected = wire(0x8003, 77, payload);
    publish(
        std::get<CompletionTarget>(fixture.sub(0).completion_target()), completion(*task, payload));
    // 无注入：真实小发送缓存/暂不读取对端产生EAGAIN，不用固定睡眠猜测拥塞。
    await([&] {
        return blocked.load() != 0 && interest.load() == EPOLLOUT;
    });
    const auto partial = sent_bytes.load(), reads = read_calls.load();
    check(partial > 0 && partial < expected.size(), "real TCP has an unsent suffix");
    auto healthy = client(fixture.port());
    for (std::uint64_t id = 100; id < 108; ++id)
        ping(healthy.get(), id);
    // PONG可先于本轮status发布；先等待报告追上八次真实往返，再断言暂停期计数。
    await([&] {
        return fixture.sub(0).status().requests == 9;
    });
    check(fixture.sub(0).status().requests == 9 && read_calls.load() == reads
              && !sink.requests.try_pop(),
        "slow TCP peer bounded, healthy peer progresses");
    check(receive(peer.get(), expected.size() + 26) == expected + wire(0x8001, 78),
        "reading resumes exact TCP suffix and cached PING");
    eof(peer.get());
    check(!fixture.stop(), "all Main handoff and completion calls quiescent before close");
    watched = -1;
    const auto report = fixture.sub(0).status();
    check(report.responses == 10 && report.output_limit_rejections == 0 && report.active == 0,
        "slow response completes once, all resources reclaimed");
}
}
int main(int argc, char **argv) {
    const auto baseline = fd_count();
    if (argc != 2)
        return 2;
    const std::string_view suite(argv[1]);
    if (suite == "limits")
        limits();
    else if (suite == "sub")
        sub_policy();
    else if (suite == "tcp")
        tcp_slow_reader();
    else
        return 2;
    check(fd_count() == baseline, "all output-policy socket and thread resources reclaimed");
    std::cout << "output backpressure " << suite << " checks=" << checks << " failures=0\n";
}
