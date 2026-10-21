/*
 * PROJECT : SIFTWING
 * FILE    : completion_wakeup_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 定点通知/锁故障、单次唤醒预算继续与提交后晚到write的停止屏障
 */
#include "../support/completion_fixture.hpp"
#include <sys/epoll.h>
#include <pthread.h>
namespace {
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool value, const char *why) {
    require(value, why, checks);
}
thread_local int write_error = 0, interruptions = 0;
thread_local bool lock_failure = false, late_writer = false;
thread_local unsigned int write_calls = 0;
std::atomic<int> watched_fd{-1};
std::atomic<bool> paused{false}, wait_entered{false}, late_entered{false}, allow_late{false};
std::atomic<CompletionQueue *> observed_queue{nullptr};
std::atomic<unsigned int> illegal_wait{0}, nonblocking_wait{0};
}
extern "C" {
ssize_t __real_write(int, const void *, std::size_t);
ssize_t __wrap_write(int fd, const void *p, std::size_t n) {
    if (fd == watched_fd.load()) {
        ++write_calls;
        if (late_writer) {
            late_entered = true;
            while (!allow_late.load())
                std::this_thread::yield();
        }
        if (interruptions > 0) {
            --interruptions;
            errno = EINTR;
            return -1;
        }
        if (write_error) {
            errno = write_error;
            return -1;
        }
    }
    return __real_write(fd, p, n);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
    if (lock_failure) {
        lock_failure = false;
        return EAGAIN;
    }
    return __real_pthread_mutex_lock(m);
}
int __real_epoll_wait(int, epoll_event *, int, int);
int __wrap_epoll_wait(int fd, epoll_event *events, int max, int timeout) {
    if (paused.load()) {
        wait_entered = true;
        while (paused.load())
            std::this_thread::yield();
    } else if (auto *queue = observed_queue.load()) {
        // 观察期无新生产者；队列非空时必须0等待，不能依赖已清掉的通知再次唤醒。
        if (queue->status().size) {
            if (timeout != 0)
                ++illegal_wait;
            else
                ++nonblocking_wait;
        }
    }
    return __real_epoll_wait(fd, events, max, timeout);
}
}
int main() {
    const auto baseline = fd_count();
    {
        CompletionQueue queue(7, 1);
        auto wake = std::get<UniqueFd>(create_eventfd_wakeup());
        watched_fd = wake.get();
        auto task = fabricated_task({7, 0, 1});
        auto result = completion(*task);
        const auto *pointer = result.get();
        lock_failure = true;
        bool failed = false;
        write_calls = 0;
        try {
            (void)try_publish_and_notify(queue, result, wake.get());
        } catch (const std::system_error &error) {
            failed = error.code().value() == EAGAIN;
        }
        check(failed && result.get() == pointer && queue.status().size == 0 && write_calls == 0,
            "commit lock error preserves source, does not notify");
        write_error = EIO;
        const auto receipt = try_publish_and_notify(queue, result, wake.get());
        check(receipt.publish == CompletionPublishResult::accepted
                  && receipt.notification_error.value() == EIO && !result
                  && queue.status().size == 1,
            "failed notification cannot revoke accepted owner");
        auto full = completion(*task);
        write_calls = 0;
        check(
            try_publish_and_notify(queue, full, wake.get()).publish == CompletionPublishResult::full
                && full && write_calls == 0,
            "full does not notify");
        auto wrong = completion(*fabricated_task({8, 0, 1}));
        check(try_publish_and_notify(queue, wrong, wake.get()).publish
                      == CompletionPublishResult::wrong_reactor
                  && wrong && write_calls == 0,
            "wrong owner does not notify");
        std::unique_ptr<RoutedCompletion> empty;
        check(try_publish_and_notify(queue, empty, wake.get()).publish
                      == CompletionPublishResult::invalid_completion
                  && write_calls == 0,
            "invalid source does not notify");
        auto popped = queue.try_pop();
        check(popped.get() == pointer, "committed result still owned exactly once");
        write_error = 0;
        interruptions = 2;
        write_calls = 0;
        auto next = completion(*task);
        const auto retried = try_publish_and_notify(queue, next, wake.get());
        check(retried.publish == CompletionPublishResult::accepted && !retried.notification_error
                  && !next && write_calls == 3,
            "finite EINTR retry publishes only once");
        (void)queue.try_pop();
        interruptions = static_cast<int>(eventfd_attempt_limit);
        write_calls = 0;
        next = completion(*task);
        const auto interrupted = try_publish_and_notify(queue, next, wake.get());
        check(interrupted.publish == CompletionPublishResult::accepted
                  && interrupted.notification_error.value() == EINTR && !next
                  && write_calls == eventfd_attempt_limit,
            "notification exhaustion retains commit");
        queue.close();
        write_calls = 0;
        check(try_publish_and_notify(queue, full, wake.get()).publish
                      == CompletionPublishResult::closed
                  && full && write_calls == 0,
            "closed source retained without notification");
        watched_fd = -1;
    }
    {
        // 单个聚合通知对应64条目；completion_budget=1，余量必须主动继续。
        ProtocolConnectionConfig config;
        config.reactor_id = 7;
        config.completion_capacity = 64;
        config.completion_budget = 1;
        ConnectionHarness sub(config);
        paused = true;
        wait_entered = false;
        check(!sub.start(), "budget observer 连接核心 ready");
        await([] {
            return wait_entered.load();
        });
        auto target = std::get<CompletionTarget>(sub.completion_target());
        for (std::uint64_t id = 1; id <= 64; ++id) {
            auto task = fabricated_task({7, 0, id}, id);
            auto value = completion(*task);
            check(target.queue->try_publish(value) == CompletionPublishResult::accepted && !value,
                "prepare real queue independently of notification count");
        }
        observed_queue = target.queue;
        check(!notify_eventfd(target.wakeup_fd), "exactly one aggregate notification");
        paused = false;
        await([&] {
            return sub.status().completion_stale == 64;
        });
        observed_queue = nullptr;
        check(illegal_wait == 0 && nonblocking_wait >= 62,
            "nonempty queue never blocks after notification drain");
        stop_core(sub);
    }
    {
        CaptureSubmitter sink;
        ProtocolConnectionConfig config;
        config.reactor_id = 7;
        config.request_submitter = &sink;
        ConnectionHarness sub(config);
        check(!sub.start(), "notification failure fallback 连接核心 ready");
        auto target = std::get<CompletionTarget>(sub.completion_target());
        watched_fd = target.wakeup_fd;
        auto peer = attach(sub);
        send_all(peer.get(), wire(3, 77));
        auto task = take_request(sink);
        auto result = completion(*task);
        write_error = EIO;
        auto receipt = try_publish_and_notify(*target.queue, result, target.wakeup_fd);
        write_error = 0;
        check(receipt.publish == CompletionPublishResult::accepted
                  && receipt.notification_error.value() == EIO && !result,
            "连接核心 queue commit survives real notification failure");
        const auto expected = wire(0x8003, 77, "{\"test\":true}");
        check(receive(peer.get(), expected.size()) == expected,
            "finite wait fallback consumes result even without notification");
        watched_fd = -1;
        stop_core(sub);
    }
    {
        ProtocolConnectionConfig config;
        config.reactor_id = 7;
        ConnectionHarness sub(config);
        check(!sub.start(), "late notification 连接核心 ready");
        auto target = std::get<CompletionTarget>(sub.completion_target());
        watched_fd = target.wakeup_fd;
        late_entered = false;
        allow_late = false;
        std::atomic<bool> committed{false};
        std::thread producer([&] {
            auto task = fabricated_task({7, 0, 1});
            auto value = completion(*task);
            late_writer = true;
            const auto receipt = try_publish_and_notify(*target.queue, value, target.wakeup_fd);
            committed = receipt.publish == CompletionPublishResult::accepted
                        && !receipt.notification_error && !value;
            late_writer = false;
        });
        await([] {
            return late_entered.load();
        });
        check(!sub.request_stop(), "stop after commit but before full publish call returns");
        await([&] {
            return sub.status().state == ReactorState::quiescing && target.queue->status().closed;
        });
        check(::fcntl(target.wakeup_fd, F_GETFD) >= 0 && target.queue->status().closed,
            "closed queue does not allow notification fd close before producer barrier");
        allow_late = true;
        producer.join();
        check(committed && !notify_eventfd(target.wakeup_fd),
            "late worker and final notification return before release");
        watched_fd = -1;
        sub.release_publishers();
        check(!sub.join(), "late publisher barrier joined");
        const auto report = sub.status();
        check(report.completion_received + report.completion_discarded == 1
                  && report.completion_applied == 0,
            "late result consumed or discarded exactly once on 连接核心");
        check(::fcntl(target.wakeup_fd, F_GETFD) == -1 && errno == EBADF,
            "notification fd closed only after barrier");
    }
    check(fd_count() == baseline, "all failure and barrier resources reclaimed");
    std::cout << "completion wakeup failure checks=" << checks << '\n';
}
