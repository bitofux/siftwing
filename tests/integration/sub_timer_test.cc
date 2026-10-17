/*
 * PROJECT : SIFTWING
 * FILE    : sub_timer_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 正式ProtocolSub每线程单timer、真实唤醒和失败回滚的所有权观测
 */
#include "../support/multi_reactor_fixture.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <fcntl.h>
#include <sys/timerfd.h>

namespace {
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool ok, const char *label) {
    require(ok, label, checks);
}
struct Record {
    int fd = -1;
    std::thread::id owner;
    bool closed = false, added = false, removed = false;
    unsigned int reads = 0;
};
std::mutex observer;
std::array<Record, 4> records;
std::size_t used = 0;
bool valid = true;
std::atomic<int> phase{0};
std::atomic<unsigned int> successful_reads{0}, injected{0};
const auto controller = std::this_thread::get_id();
Record *find(int fd) {
    for (std::size_t i = 0; i < used; ++i)
        if (records[i].fd == fd && !records[i].closed)
            return &records[i];
    return nullptr;
}
void reset(int selected) {
    const std::lock_guard<std::mutex> lock(observer);
    for (std::size_t i = 0; i < used; ++i)
        check(records[i].closed, "previous timer reclaimed");
    check(valid, "previous timer operations on owner thread");
    records = {};
    used = 0;
    valid = true;
    phase = selected;
    successful_reads = 0;
    injected = 0;
}
void verify(std::size_t count) {
    const std::lock_guard<std::mutex> lock(observer);
    check(valid && used == count, "exactly one timer per started Sub and owner thread");
    for (std::size_t i = 0; i < used; ++i)
        check(records[i].closed && (!records[i].added || records[i].removed),
            "unregister before one close");
}
} // namespace
extern "C" {
int __real_timerfd_create(int, int);
int __wrap_timerfd_create(int clock, int flags) {
    if (phase.load() == 2) {
        ++injected;
        errno = ENFILE;
        return -1;
    }
    const int fd = __real_timerfd_create(clock, flags);
    if (fd >= 0) {
        const std::lock_guard<std::mutex> lock(observer);
        if (used == records.size())
            std::exit(2);
        valid = valid && clock == CLOCK_MONOTONIC && flags == (TFD_NONBLOCK | TFD_CLOEXEC)
                && std::this_thread::get_id() != controller;
        records[used].fd = fd;
        records[used].owner = std::this_thread::get_id();
        ++used;
    }
    return fd;
}
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_ctl(int fd, int op, int target, epoll_event *event) {
    const std::lock_guard<std::mutex> lock(observer);
    auto *record = find(target);
    if (!record)
        return __real_epoll_ctl(fd, op, target, event);
    valid = valid && record->owner == std::this_thread::get_id();
    if (op == EPOLL_CTL_ADD) {
        valid = valid && event && event->data.u64 == 0 && event->events == EPOLLIN;
        itimerspec current{};
        valid = valid && ::timerfd_gettime(target, &current) == 0 && current.it_value.tv_sec == 0
                && current.it_value.tv_nsec == 0;
        if (phase.load() == 3) {
            ++injected;
            errno = EIO;
            return -1;
        }
        const int result = __real_epoll_ctl(fd, op, target, event);
        record->added = result == 0;
        if (result == 0 && (phase.load() == 1 || phase.load() == 4)) {
            // 仅本测试静态包装在所属线程注入已过期单次设时；生产无超时策略/测试钩子。
            itimerspec expired{};
            expired.it_value.tv_nsec = 1;
            valid = valid && ::timerfd_settime(target, TFD_TIMER_ABSTIME, &expired, nullptr) == 0;
        }
        return result;
    }
    const int result = __real_epoll_ctl(fd, op, target, event);
    if (op == EPOLL_CTL_DEL) {
        record->removed = result == 0;
        if (phase.load() == 5) {
            ++injected;
            errno = EBUSY;
            return -1;
        }
    }
    return result;
}
ssize_t __real_read(int, void *, std::size_t);
ssize_t __wrap_read(int fd, void *bytes, std::size_t size) {
    const std::lock_guard<std::mutex> lock(observer);
    auto *record = find(fd);
    if (record) {
        valid = valid && record->owner == std::this_thread::get_id();
        if (phase.load() == 4) {
            ++injected;
            errno = EIO;
            return -1;
        }
    }
    const auto result = __real_read(fd, bytes, size);
    if (record && result == static_cast<ssize_t>(sizeof(std::uint64_t))) {
        ++record->reads;
        ++successful_reads;
    }
    return result;
}
int __real_close(int);
int __wrap_close(int fd) {
    const std::lock_guard<std::mutex> lock(observer);
    auto *record = find(fd);
    if (record) {
        valid = valid && record->owner == std::this_thread::get_id();
        record->closed = true;
    }
    const int result = __real_close(fd);
    if (record && phase.load() == 4) {
        ++injected;
        errno = EINTR;
        return -1;
    }
    return result;
}
}
int main() {
    const auto baseline = fd_count();
    for (const int selected : {0, 1}) {
        reset(selected);
        {
            MultiReactorFixture fixture(2);
            check(!fixture.start(), "two protocol Subs ready");
            if (selected == 1)
                await([] {
                    return successful_reads.load() == 2;
                });
            for (std::uint64_t id = 1; id <= 8; ++id) {
                auto peer = client(fixture.port());
                send_all(peer.get(), wire(1, id));
                check(receive(peer.get(), 26) == wire(0x8001, id),
                    "TCP PING survives timer control events");
            }
            check(!fixture.stop(), "normal stop and join");
            check(successful_reads.load() == (selected == 1 ? 2U : 0U),
                "single-shot each fires exactly once or stays disarmed");
            verify(2);
        }
        check(fd_count() == baseline, "normal timer fds return to baseline");
    }
    for (const int selected : {2, 3, 4, 5}) {
        reset(selected);
        {
            ProtocolSubReactor sub;
            const auto started = sub.start();
            if (selected == 2 || selected == 3) {
                check(started.value() == (selected == 2 ? ENFILE : EIO),
                    "create or ADD failure preserves startup cause");
                check(sub.status().state == ProtocolSubState::joined,
                    "failed startup thread already joined");
                check(std::holds_alternative<std::error_code>(sub.target()),
                    "failed startup never publishes target");
            } else {
                check(!started, "ready before runtime or cleanup failure");
                if (selected == 4)
                    await([&] {
                        return sub.status().first_error.value() == EIO;
                    });
                (void)sub.request_stop();
                sub.release_publishers();
                check(sub.join().value() == (selected == 4 ? EIO : EBUSY),
                    "runtime cause wins over independent cleanup cause");
                const auto status = sub.status();
                check(selected == 4 ? status.first_error.value() == EIO
                                          && status.cleanup_error.value() == EINTR
                                    : !status.first_error && status.cleanup_error.value() == EBUSY,
                    "separate run and cleanup diagnostics");
            }
        }
        check(injected.load() == (selected == 4 ? 2U : 1U), "exact fault injection count");
        verify(selected == 2 ? 0 : 1);
        check(fd_count() == baseline, "failure rollback returns all fds to baseline");
    }
    std::cout << "Sub timer checks=" << checks << '\n';
}
