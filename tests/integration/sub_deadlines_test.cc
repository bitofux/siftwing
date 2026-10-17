/*
 * PROJECT : SIFTWING
 * FILE    : sub_deadlines_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 独立同钟边界、真实timer唤醒、请求/输出期限与旧结果隔离
 */
#include "../support/completion_fixture.hpp"
#include <sys/timerfd.h>
#include <sys/epoll.h>
namespace {
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool value, const char *why) {
    require(value, why, checks);
}
std::atomic<std::int64_t> fake_time{0};
std::atomic<unsigned int> arms{0}, reads{0}, blocked_writes{0};
std::atomic<std::int64_t> advance_after_clock{0}, advance_after_send{0};
std::atomic<bool> pause_wait{false}, waiting{false}, release_wait{false};
std::atomic<bool> fail_clock{false}, fail_arm{false};
const auto controller = std::this_thread::get_id();
constexpr std::int64_t millisecond = 1000000;
}
extern "C" {
int __real_clock_gettime(clockid_t, timespec *);
int __wrap_clock_gettime(clockid_t clock, timespec *value) {
    if (clock == CLOCK_MONOTONIC) {
        if (fail_clock.exchange(false)) {
            errno = EIO;
            return -1;
        }
        const auto time = fake_time.load();
        if (const auto next = advance_after_clock.exchange(0))
            fake_time = next;
        if (time) {
            value->tv_sec = static_cast<time_t>(time / 1000000000);
            value->tv_nsec = static_cast<long>(time % 1000000000);
            return 0;
        }
    }
    return __real_clock_gettime(clock, value);
}
int __real_timerfd_settime(int, int, const itimerspec *, itimerspec *);
int __wrap_timerfd_settime(int fd, int flags, const itimerspec *value, itimerspec *old) {
    if (fail_arm.exchange(false)) {
        errno = EIO;
        return -1;
    }
    const int result = __real_timerfd_settime(fd, flags, value, old);
    if (result == 0)
        ++arms;
    return result;
}
ssize_t __real_recv(int, void *, std::size_t, int);
ssize_t __wrap_recv(int fd, void *data, std::size_t size, int flags) {
    const auto result = __real_recv(fd, data, size, flags);
    if (result > 0 && std::this_thread::get_id() != controller)
        ++reads;
    return result;
}
ssize_t __real_send(int, const void *, std::size_t, int);
ssize_t __wrap_send(int fd, const void *data, std::size_t size, int flags) {
    const auto result = __real_send(fd, data, size, flags);
    if (result > 0 && std::this_thread::get_id() != controller)
        if (const auto next = advance_after_send.exchange(0))
            fake_time = next;
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
        && std::this_thread::get_id() != controller)
        ++blocked_writes;
    return result;
}
int __real_epoll_wait(int, epoll_event *, int, int);
int __wrap_epoll_wait(int fd, epoll_event *events, int count, int timeout) {
    const auto result = __real_epoll_wait(fd, events, count, timeout);
    if (pause_wait.exchange(false)) {
        waiting = true;
        while (!release_wait.load())
            std::this_thread::yield();
    }
    return result;
}
}
namespace {
std::int64_t freeze() {
    timespec value{};
    check(__real_clock_gettime(CLOCK_MONOTONIC, &value) == 0, "sample actual monotonic clock");
    // 放在真实内核时间一小时之后；用eventfd推进确定性观察，另有不包装时间的真实timer测试。
    const auto base =
        static_cast<std::int64_t>(value.tv_sec) * 1000000000 + value.tv_nsec + 3600000000000;
    fake_time = base;
    return base;
}
void pulse(ProtocolSubReactor &sub) {
    // arm发生在wait前；两轮唤醒保证前一轮业务与报告发布都完成。
    for (int round = 0; round < 2; ++round) {
        const auto previous = arms.load();
        check(!notify_eventfd(std::get<MainReactorTarget>(sub.target()).wakeup_fd), "wake owner");
        await([&] {
            return arms.load() > previous;
        });
    }
}
void admitted(ProtocolSubReactor &sub, std::size_t count = 1) {
    await([&] {
        return sub.status().admitted == count;
    });
}
void idle_progress() {
    const auto base = freeze();
    ProtocolSubConfig config;
    config.idle_timeout_ms = 1000;
    config.max_connections = 2;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "idle Sub ready");
    auto peer = attach(sub);
    admitted(sub);
    fake_time = base + 500 * millisecond;
    const auto previous = reads.load();
    send_all(peer.get(), wire(1, 7).substr(0, 1));
    await([&] {
        return reads.load() > previous;
    });
    pulse(sub); // 先确认刷新已提交，再推进到旧deadline之后。
    fake_time = base + 1499 * millisecond;
    pulse(sub);
    check(sub.status().active == 1 && sub.status().requests == 0,
        "partial recv refreshes idle, not request");
    fake_time = base + 1500 * millisecond;
    check(!notify_eventfd(std::get<MainReactorTarget>(sub.target()).wakeup_fd), "wake equality");
    await([&] {
        return sub.status().idle_timeouts == 1;
    });
    check(sub.status().active == 0 && sub.status().first_connection_error.value() == ETIMEDOUT,
        "idle equality closes only connection");
    // 相同slot重新接纳，旧条目已真实取消，不继承旧绝对期限。
    auto fresh = attach(sub);
    admitted(sub, 2);
    send_all(fresh.get(), wire(1, 8));
    check(receive(fresh.get(), 26) == wire(0x8001, 8),
        "reused slot stays healthy after old idle timeout");
    stop_sub(sub);
}
void request_window() {
    const auto base = freeze();
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.request_timeout_ms = 1000;
    config.request_submitter = &sink;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "request Sub ready");
    auto target = std::get<CompletionTarget>(sub.completion_target());
    auto peer = attach(sub);
    admitted(sub);
    const auto request = wire(3, 77);
    const auto previous = reads.load();
    send_all(peer.get(), request.substr(0, 1));
    await([&] {
        return reads.load() > previous;
    });
    pulse(sub);
    fake_time = base + 5000 * millisecond;
    send_all(peer.get(), request.substr(1));
    auto task = take_request(sink);
    pulse(sub);
    check(sub.status().request_timeouts == 0, "deadline starts at full frame, not first byte");
    fake_time = base + 5999 * millisecond;
    publish(target, completion(*task));
    const auto response = wire(0x8003, 77, "{\"test\":true}");
    check(receive(peer.get(), response.size()) == response,
        "completion strictly before deadline accepted");
    await([&] {
        return sub.status().responses == 1;
    });
    // 完成后复用request_id合法，新窗口从第二帧开始；到期关闭身份而非发ERROR后复用。
    send_all(peer.get(), request);
    auto second = take_request(sink);
    pulse(sub);
    fake_time = base + 6999 * millisecond;
    publish(target, completion(*second));
    await([&] {
        return sub.status().request_timeouts == 1 && sub.status().completion_received == 2;
    });
    check(sub.status().completion_applied == 1 && sub.status().responses == 1
              && sub.status().active == 0,
        "equal deadline cannot apply or finish second request");
    publish(target, completion(*second));
    await([&] {
        return sub.status().completion_received == 3;
    });
    check(sub.status().completion_stale >= 1 && sub.status().request_timeouts == 1,
        "late completion stale, no double timeout");
    auto healthy = attach(sub);
    admitted(sub, 2);
    send_all(healthy.get(), wire(1, 88));
    check(receive(healthy.get(), 26) == wire(0x8001, 88),
        "other/new connection progresses after request timeout");
    stop_sub(sub);
}
void pending_and_half_close() {
    const auto base = freeze();
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.idle_timeout_ms = 100;
    config.request_timeout_ms = 1000;
    config.request_submitter = &sink;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "two timers configured");
    auto peer = attach(sub);
    admitted(sub);
    send_all(peer.get(), wire(3, 21));
    check(::shutdown(peer.get(), SHUT_WR) == 0, "half close request source");
    auto task = take_request(sink);
    pulse(sub);
    fake_time = base + 500 * millisecond;
    pulse(sub);
    check(sub.status().active == 1 && sub.status().idle_timeouts == 0,
        "pending request suspends shorter idle timer even with SHUT_WR");
    publish(std::get<CompletionTarget>(sub.completion_target()), completion(*task));
    const auto expected = wire(0x8003, 21, "{\"test\":true}");
    check(receive(peer.get(), expected.size()) == expected,
        "half closed peer gets result before request deadline");
    await([&] {
        return sub.status().responses == 1 && sub.status().active == 0;
    });
    check(sub.status().idle_timeouts == 0 && sub.status().request_timeouts == 0,
        "EOF cancels timer without timeout");
    stop_sub(sub);
}
void slow_output() {
    const auto base = freeze();
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.request_timeout_ms = 1000;
    config.request_submitter = &sink;
    config.write_byte_budget = 300000;
    config.io_attempt_budget = 64;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "slow output Sub ready");
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
        "slow peer pair");
    UniqueFd source(pair[0]), peer(pair[1]);
    int size = 4096;
    check(::setsockopt(source.get(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0,
        "small sender buffer");
    auto destination = std::get<MainReactorTarget>(sub.target());
    check(try_handoff_and_notify(*destination.queue, source, destination.wakeup_fd).handoff
              == FdHandoffResult::accepted,
        "slow source admitted");
    admitted(sub);
    send_all(peer.get(), wire(3, 31) + wire(1, 32));
    auto task = take_request(sink);
    const auto previous = blocked_writes.load();
    const std::string body = "{\"text\":\"" + std::string(262144, 'x') + "\"}";
    publish(std::get<CompletionTarget>(sub.completion_target()), completion(*task, body));
    await([&] {
        return blocked_writes.load() > previous;
    });
    pulse(sub);
    await([&] {
        return sub.status().completion_applied == 1;
    });
    check(sub.status().responses == 0, "result application is not output completion");
    fake_time = base + 1000 * millisecond;
    check(!notify_eventfd(destination.wakeup_fd), "wake stalled output deadline");
    await([&] {
        return sub.status().request_timeouts == 1;
    });
    check(sub.status().responses == 0 && sub.status().requests == 1 && sub.status().active == 0,
        "partial output times out, cached next frame never admitted");
    stop_sub(sub);
}
// 固定在wait返回后暂停：本批timer扫描观察期限前一刻，下一次complete独立观察等号。
void completion_before_timer() {
    const auto base = freeze();
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.request_timeout_ms = 1000;
    config.request_submitter = &sink;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "completion order Sub ready");
    auto peer = attach(sub);
    admitted(sub);
    send_all(peer.get(), wire(3, 51));
    auto task = take_request(sink);
    pulse(sub);
    waiting = false;
    release_wait = false;
    pause_wait = true;
    auto destination = std::get<MainReactorTarget>(sub.target());
    check(!notify_eventfd(destination.wakeup_fd), "pause returning control wait");
    await([] {
        return waiting.load();
    });
    publish(std::get<CompletionTarget>(sub.completion_target()), completion(*task));
    fake_time = base + 999 * millisecond;
    advance_after_clock = base + 1000 * millisecond;
    release_wait = true;
    await([&] {
        return sub.status().completion_received == 1;
    });
    check(sub.status().completion_rejected == 1 && sub.status().completion_applied == 0
              && sub.status().request_timeouts == 1,
        "completion gate enforces equality before timer scan sees due");
    stop_sub(sub);
}
void drain_and_idle_restart() {
    const auto base = freeze();
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.idle_timeout_ms = 100;
    config.request_timeout_ms = 1000;
    config.request_submitter = &sink;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "idle restart Sub ready");
    auto peer = attach(sub);
    admitted(sub);
    send_all(peer.get(), wire(3, 61));
    auto task = take_request(sink);
    pulse(sub);
    fake_time = base + 500 * millisecond;
    publish(std::get<CompletionTarget>(sub.completion_target()), completion(*task));
    const auto response = wire(0x8003, 61, "{\"test\":true}");
    check(receive(peer.get(), response.size()) == response, "pending idle suspended until drain");
    await([&] {
        return sub.status().responses == 1;
    });
    fake_time = base + 599 * millisecond;
    pulse(sub);
    check(sub.status().active == 1, "new idle deadline starts at drain");
    fake_time = base + 600 * millisecond;
    check(!notify_eventfd(std::get<MainReactorTarget>(sub.target()).wakeup_fd),
        "wake new idle equality");
    await([&] {
        return sub.status().idle_timeouts == 1;
    });
    check(sub.status().request_timeouts == 0, "completed request no longer has request deadline");
    stop_sub(sub);
}
void send_crosses_deadline() {
    const auto base = freeze();
    ProtocolSubConfig config;
    config.request_timeout_ms = 1000;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "send crossing Sub ready");
    auto peer = attach(sub);
    admitted(sub);
    advance_after_send = base + 1000 * millisecond;
    send_all(peer.get(), wire(1, 71));
    await([&] {
        return sub.status().request_timeouts == 1;
    });
    check(sub.status().responses == 0 && sub.status().active == 0,
        "send crossing equality cannot report timely finish even if bytes reached kernel");
    stop_sub(sub);
}
void same_time_batch() {
    const auto base = freeze();
    ProtocolSubConfig config;
    config.idle_timeout_ms = 1000;
    config.max_connections = 3;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "bounded batch Sub ready");
    auto first = attach(sub), second = attach(sub), third = attach(sub);
    admitted(sub, 3);
    fake_time = base + 1000 * millisecond;
    check(!notify_eventfd(std::get<MainReactorTarget>(sub.target()).wakeup_fd),
        "wake equal deadline batch");
    await([&] {
        return sub.status().idle_timeouts == 3;
    });
    check(sub.status().active == 0, "one control wake consumes all due entries within capacity");
    stop_sub(sub);
}
void real_timer() {
    fake_time = 0;
    ProtocolSubConfig config;
    config.idle_timeout_ms = 50;
    MultiReactorFixture fixture(2, config);
    check(!fixture.start(), "real timer TCP fixture ready");
    auto first = client(fixture.port()), second = client(fixture.port());
    await([&] {
        return fixture.sub(0).status().idle_timeouts + fixture.sub(1).status().idle_timeouts == 2;
    });
    check(fixture.sub(0).status().active + fixture.sub(1).status().active == 0,
        "two real TCP idle connections expire without injected control wake");
    check(!fixture.stop(), "local timeouts preserve server lifecycle");
}
void infrastructure_failure(bool clock) {
    freeze();
    ProtocolSubConfig config;
    config.idle_timeout_ms = 1000;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "failure Sub initially ready");
    auto peer = attach(sub);
    admitted(sub);
    auto destination = std::get<MainReactorTarget>(sub.target());
    if (clock)
        fail_clock = true;
    else
        fail_arm = true;
    check(!notify_eventfd(destination.wakeup_fd), "trigger infrastructure failure");
    await([&] {
        return sub.status().first_error.value() == EIO;
    });
    check(sub.status().state == ProtocolSubState::quiescing,
        "time infrastructure failure stops Sub with first cause");
    (void)sub.request_stop();
    sub.release_publishers();
    check(sub.join().value() == EIO && sub.status().active == 0,
        "failure cleanup preserves first cause");
}
}
int main() {
    const auto baseline = fd_count();
    for (const auto value : {-1LL, 86400001LL})
        for (bool request : {false, true}) {
            ProtocolSubConfig config;
            (request ? config.request_timeout_ms : config.idle_timeout_ms) = value;
            bool rejected = false;
            try {
                ProtocolSubReactor invalid(config);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            check(rejected, "duration bounds rejected before resources");
        }
    idle_progress();
    request_window();
    pending_and_half_close();
    slow_output();
    real_timer();
    completion_before_timer();
    drain_and_idle_restart();
    send_crosses_deadline();
    same_time_batch();
    infrastructure_failure(true);
    infrastructure_failure(false);
    fake_time = std::numeric_limits<std::int64_t>::max() - 1000;
    {
        ProtocolSubConfig config;
        config.idle_timeout_ms = 1;
        ProtocolSubReactor sub(config);
        check(!sub.start(), "overflow starts with no active deadline");
        auto peer = attach(sub);
        await([&] {
            return sub.status().first_error.value() == EOVERFLOW;
        });
        (void)sub.request_stop();
        sub.release_publishers();
        check(sub.join().value() == EOVERFLOW && sub.status().active == 0,
            "deadline addition overflow stops safely");
    }
    fake_time = 0;
    check(fd_count() == baseline, "all timers, sockets and control fds reclaimed");
    std::cout << checks << " deadline policy checks passed\n";
}
