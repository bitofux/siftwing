/*
 * PROJECT : SIFTWING
 * FILE    : graceful_sub_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 排空边界、慢输出、原期限与完整发布屏障的独立证明
 */
#include "../support/completion_fixture.hpp"
#include "siftwing/net/deadline_timer.hpp"
#include <sys/timerfd.h>
#include <sys/epoll.h>

namespace {
using namespace siftwing::test;
unsigned int checks = 0;
std::atomic<std::int64_t> fake_time{0}, after_send{0};
std::atomic<int> watched{-1};
std::atomic<bool> block_send{false}, fail_notify{false}, fail_arm{false}, fail_clock{false};
std::atomic<unsigned int> blocked{0};
std::atomic<bool> pause_wait{false}, waiting{false}, release_wait{false};
const auto controller = std::this_thread::get_id();
void check(bool ok, const char *why) {
    require(ok, why, checks);
}
}
extern "C" {
int __real_clock_gettime(clockid_t, timespec *);
int __wrap_clock_gettime(clockid_t clock, timespec *value) {
    if (clock == CLOCK_MONOTONIC && std::this_thread::get_id() != controller
        && fail_clock.exchange(false)) {
        errno = EIO;
        return -1;
    }
    if (clock == CLOCK_MONOTONIC && fake_time.load()) {
        const auto time = fake_time.load();
        value->tv_sec = static_cast<time_t>(time / 1000000000);
        value->tv_nsec = static_cast<long>(time % 1000000000);
        return 0;
    }
    return __real_clock_gettime(clock, value);
}
ssize_t __real_send(int, const void *, std::size_t, int);
ssize_t __wrap_send(int fd, const void *data, std::size_t size, int flags) {
    if (fd == watched.load() && block_send.load()) {
        ++blocked;
        errno = EAGAIN;
        return -1;
    }
    const auto result = __real_send(fd, data, size, flags);
    if (fd == watched.load() && result > 0)
        if (const auto time = after_send.exchange(0))
            fake_time = time;
    return result;
}
ssize_t __real_write(int, const void *, std::size_t);
ssize_t __wrap_write(int fd, const void *data, std::size_t size) {
    if (size == 8 && fail_notify.exchange(false)) {
        errno = EIO;
        return -1;
    }
    return __real_write(fd, data, size);
}
int __real_timerfd_settime(int, int, const itimerspec *, itimerspec *);
int __wrap_timerfd_settime(int fd, int flags, const itimerspec *value, itimerspec *old) {
    if (fail_arm.exchange(false)) {
        errno = EIO;
        return -1;
    }
    return __real_timerfd_settime(fd, flags, value, old);
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
    timespec time{};
    check(__real_clock_gettime(CLOCK_MONOTONIC, &time) == 0, "actual monotonic clock sampled");
    const auto base =
        static_cast<std::int64_t>(time.tv_sec) * 1000000000 + time.tv_nsec + 3600000000000;
    fake_time = base;
    return base;
}
void wake(int fd) {
    check(!notify_eventfd(fd), "wake owner with stable target");
}
void quiescent(ProtocolSubReactor &sub) {
    await([&] {
        return sub.status().state == ProtocolSubState::quiescing;
    });
}
void finish(ProtocolSubReactor &sub, CompletionTarget target, std::size_t active = 0) {
    quiescent(sub);
    await([&] {
        return target.queue->status().closed;
    });
    check(::fcntl(target.wakeup_fd, F_GETFD) >= 0,
        "quiescing retains complete publisher notification fd");
    check(sub.status().active == active, "quiescing connection count");
    sub.release_publishers();
    check(!sub.join(), "graceful Sub joins without service error");
    check(::fcntl(target.wakeup_fd, F_GETFD) == -1 && errno == EBADF,
        "last publisher returned then fd closed");
    check(sub.status().state == ProtocolSubState::joined && sub.status().active == 0,
        "final joined and no connections");
}
UniqueFd watched_peer(ProtocolSubReactor &sub) {
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
        "owned nonblocking socketpair");
    UniqueFd source(pair[0]), peer(pair[1]);
    watched = source.get();
    const auto target = std::get<MainReactorTarget>(sub.target());
    check(try_handoff_and_notify(*target.queue, source, target.wakeup_fd).handoff
              == FdHandoffResult::accepted,
        "real handoff consumes source");
    return peer;
}
void drain() {
    const auto base = freeze();
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.request_submitter = &sink;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "Sub ready");
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    const auto handoff = std::get<MainReactorTarget>(sub.target());
    auto peer = attach(sub);
    auto idle = attach(sub);
    auto partial = attach(sub);
    send_all(peer.get(), wire(3, 77) + wire(1, 78));
    check(::shutdown(peer.get(), SHUT_WR) == 0, "pending peer half closes");
    send_all(partial.get(), wire(1, 79).substr(0, 10));
    auto task = take_request(sink);
    await([&] {
        return sub.status().admitted == 3;
    });
    check(!sub.request_drain(base + 1000000000), "publish absolute drain request");
    handoff.queue->close(); // 模拟Main close在排空发布后关闭handoff，不能退回立即丢弃。
    await([&] {
        return sub.status().drain_started && sub.status().active == 1;
    });
    check(sub.status().state == ProtocolSubState::draining, "only pending connection remains");
    check(std::holds_alternative<std::error_code>(sub.target()),
        "draining revokes new target acquisition");
    publish(target, completion(*task));
    const auto expected = wire(0x8003, 77, "{\"test\":true}");
    check(receive(peer.get(), expected.size()) == expected,
        "exact independent success wire before close");
    finish(sub, target);
    check(sub.status().requests == 1 && sub.status().responses == 1
              && sub.status().drain_responses == 1 && sub.status().drain_timeouts == 0
              && sink.requests.status().size == 0,
        "cached PING and half packet are never accepted during drain");
}
void queued_handoffs() {
    const auto base = freeze();
    ProtocolSubConfig config;
    config.handoff_budget = 1;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "handoff drain Sub ready");
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    const auto input = std::get<MainReactorTarget>(sub.target());
    // 暂停真实epoll返回，确保这三项都仍由handoff拥有，没有机会注册Connection。
    waiting = false;
    release_wait = false;
    pause_wait = true;
    wake(input.wakeup_fd);
    await([] {
        return waiting.load();
    });
    std::array<UniqueFd, 3> peers;
    std::array<int, 3> numbers{};
    for (std::size_t n = 0; n < peers.size(); ++n) {
        int pair[2];
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
            "queued handoff peer");
        UniqueFd source(pair[0]);
        peers[n] = UniqueFd(pair[1]);
        numbers[n] = source.get();
        check(try_handoff_and_notify(*input.queue, source, input.wakeup_fd).handoff
                  == FdHandoffResult::accepted,
            "handoff owns queued source");
    }
    check(input.queue->status().size == 3, "all sources confirmed queued before drain");
    check(!sub.request_drain(base + 1000000000), "drain before admitting queued sources");
    release_wait = true;
    quiescent(sub);
    check(sub.status().discarded == 3 && sub.status().received == 0 && sub.status().admitted == 0,
        "queued handoffs closed before publisher barrier, never admitted");
    for (std::size_t n = 0; n < peers.size(); ++n) {
        check(::fcntl(numbers[n], F_GETFD) == -1 && errno == EBADF,
            "queued source fd closed while publisher notification fd remains");
        char byte;
        check(::recv(peers[n].get(), &byte, 1, MSG_DONTWAIT) == 0,
            "unadmitted peer observes immediate close");
    }
    finish(sub, target);
}
void cutoff(bool send_crosses, bool earlier_request) {
    const auto base = freeze();
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.request_submitter = &sink;
    if (earlier_request)
        config.request_timeout_ms = 1;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "cutoff Sub ready");
    auto peer = watched_peer(sub);
    send_all(peer.get(), wire(3, 11) + wire(1, 12));
    auto task = take_request(sink);
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    const auto deadline = base + (earlier_request ? 2000000 : 1000000);
    check(!sub.request_drain(deadline), "begin bounded drain");
    await([&] {
        return sub.status().drain_started;
    });
    check(sub.request_drain(0).value() == EINVAL && sub.request_drain(-1).value() == EINVAL,
        "invalid drain deadline preserves existing window");
    check(!sub.request_drain(deadline + 1000000000), "repeated later deadline cannot extend");
    if (send_crosses) {
        after_send = deadline;
        publish(target, completion(*task));
    } else if (!earlier_request) {
        block_send = true;
        const auto before = blocked.load();
        publish(target, completion(*task));
        await([&] {
            return blocked.load() > before;
        });
        fake_time = deadline;
        wake(target.wakeup_fd);
    } else {
        fake_time = base + 1000000;
        wake(target.wakeup_fd);
    }
    finish(sub, target);
    block_send = false;
    watched = -1;
    const auto state = sub.status();
    check(state.responses == 0 && state.drain_responses == 0 && state.requests == 1,
        "deadline equality prevents finish and cached request admission");
    check(state.drain_timeouts == (earlier_request ? 0U : 1U)
              && state.request_timeouts == (earlier_request ? 1U : 0U),
        "earlier request deadline remains effective");
}
void waiting_and_tightening() {
    const auto base = freeze();
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.request_submitter = &sink;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "waiting Sub ready");
    auto peer = attach(sub);
    send_all(peer.get(), wire(3, 21));
    auto task = take_request(sink);
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    check(!sub.request_drain(base + 1000000000), "first drain window");
    await([&] {
        return sub.status().drain_started;
    });
    check(!sub.request_drain(base + 1), "tighten drain window");
    fake_time = base + 1;
    wake(target.wakeup_fd);
    quiescent(sub);
    await([&] {
        return target.queue->status().closed;
    });
    auto result = completion(*task);
    auto *original = result.get();
    const auto refused = try_publish_and_notify(*target.queue, result, target.wakeup_fd);
    check(refused.publish == CompletionPublishResult::closed && result.get() == original
              && !refused.notification_error,
        "late worker result refused without consumption or notification");
    finish(sub, target);
    check(sub.status().drain_timeouts == 1, "pending worker cutoff counted once");
    check(sub.request_drain(base + 2).value() == ECANCELED, "cannot restart joined Sub");
}
void callback_crossing() {
    const auto base = freeze();
    // 最后一个提交回调中收到关闭请求并跨到等号；真实closed分支会构造STOPPING输出。
    // 验证发送前裁决，而非发送完整错误后只把统计改成timeout。
    struct ClosingSubmitter final : RoutedRequestSubmitter {
        ProtocolSubReactor *owner = nullptr;
        std::int64_t when = 0;
        TaskSubmitResult try_submit(std::unique_ptr<RoutedSearchTask> &) override {
            if (const auto error = owner->request_drain(when))
                throw std::system_error(error);
            fake_time = when;
            return TaskSubmitResult::closed;
        }
    } sink;
    sink.when = base + 1;
    ProtocolSubConfig config;
    config.request_submitter = &sink;
    ProtocolSubReactor sub(config);
    sink.owner = &sub;
    check(!sub.start(), "callback crossing Sub ready");
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    auto peer = attach(sub);
    send_all(peer.get(), wire(3, 51));
    quiescent(sub);
    char byte;
    check(::recv(peer.get(), &byte, 1, MSG_DONTWAIT) == 0,
        "callback crossing deadline closes before any STOPPING byte is sent");
    finish(sub, target);
    check(sub.status().drain_timeouts == 1 && sub.status().responses == 0,
        "closed submission does not extend or evade drain deadline");
}
void faults() {
    for (int mode = 0; mode != 3; ++mode) {
        const auto base = freeze();
        CaptureSubmitter sink;
        ProtocolSubConfig config;
        config.request_submitter = &sink;
        ProtocolSubReactor sub(config);
        check(!sub.start(), "fault Sub ready");
        auto peer = attach(sub);
        send_all(peer.get(), wire(3, 31));
        auto task = take_request(sink);
        const auto target = std::get<CompletionTarget>(sub.completion_target());
        if (mode == 0)
            fail_notify = true;
        if (mode == 1)
            fail_arm = true;
        if (mode == 2)
            fail_clock = true;
        const auto result = sub.request_drain(base + 1000000);
        if (mode == 0) {
            check(result.value() == EIO && !fail_notify.load(),
                "notification failure leaves requested drain");
            fake_time = base + 1000000;
        } else
            check(!result, "publish before owner infrastructure fault");
        quiescent(sub);
        check(::fcntl(target.wakeup_fd, F_GETFD) >= 0, "fault retains publisher fd until release");
        sub.release_publishers();
        const auto error = sub.join();
        check(mode == 0 ? !error : error.value() == EIO,
            "notification fallback or original infrastructure cause");
        check(!fail_notify.load() && !fail_arm.load() && !fail_clock.load(),
            "requested fault actually reached");
    }
}
void real_timer() {
    fake_time = 0;
    CaptureSubmitter sink;
    ProtocolSubConfig config;
    config.request_submitter = &sink;
    ProtocolSubReactor sub(config);
    check(!sub.start(), "real timer Sub ready");
    auto peer = attach(sub);
    send_all(peer.get(), wire(3, 41));
    auto task = take_request(sink);
    const auto target = std::get<CompletionTarget>(sub.completion_target());
    const auto current = std::get<DeadlineTime>(monotonic_now());
    check(!sub.request_drain(current + 20000000), "real 20ms absolute timer armed");
    finish(sub, target);
    check(sub.status().drain_timeouts == 1,
        "real timer cuts off worker wait without completion or new I/O");
}
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const auto descriptors = fd_count();
    const std::string mode = argv[1];
    if (mode == "drain") {
        drain();
        queued_handoffs();
    } else if (mode == "boundaries") {
        cutoff(false, false);
        cutoff(true, false);
        cutoff(false, true);
        waiting_and_tightening();
        callback_crossing();
    } else if (mode == "faults")
        faults();
    else if (mode == "timer")
        real_timer();
    else
        return 2;
    check(fd_count() == descriptors, "all owned descriptors returned after each suite");
    std::cout << checks << " graceful Sub checks passed\n";
}
