/*
 * PROJECT : SIFTWING
 * FILE    : epoll_failure_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 仅测试链接器wrap验证epoll单次调用、errno与失败后的原注册
 */
#include "siftwing/net/epoll_ops.hpp"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/socket.h>
#include <utility>

namespace {
using namespace siftwing::net;
unsigned int checks = 0, create_calls = 0, control_calls = 0;
int create_error = 0, control_error = 0;
int flags = 0, operation = 0, observed_epoll = -1, observed_fd = -1;
bool null_event = false;
epoll_event observed{};
void require(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        std::cerr << "epoll injection failed: " << label << '\n';
        std::exit(1);
    }
}
bool error_is(std::error_code error, int value) {
    return error.value() == value && error.category() == std::generic_category();
}
} // namespace
extern "C" {
int __real_epoll_create1(int);
int __real_epoll_ctl(int, int, int, epoll_event *);
int __wrap_epoll_create1(int input_flags) {
    ++create_calls;
    flags = input_flags;
    if (create_error) {
        errno = create_error;
        return -1;
    }
    return __real_epoll_create1(input_flags);
}
int __wrap_epoll_ctl(int epoll_fd, int op, int fd, epoll_event *event) {
    ++control_calls;
    observed_epoll = epoll_fd;
    observed_fd = fd;
    operation = op;
    null_event = event == nullptr;
    if (event)
        observed = *event;
    if (control_error) {
        errno = control_error;
        return -1;
    }
    return __real_epoll_ctl(epoll_fd, op, fd, event);
}
}
int main() {
    for (const int failure : {EMFILE, ENFILE, ENOMEM, EINVAL, EINTR}) {
        create_error = failure;
        const auto before = create_calls;
        auto result = create_epoll();
        require(std::holds_alternative<std::error_code>(result)
                    && error_is(std::get<std::error_code>(result), failure),
            "create forwards errno without partial owner");
        require(create_calls == before + 1 && flags == EPOLL_CLOEXEC, "one flagged create");
    }
    create_error = 0;
    auto result = create_epoll();
    require(std::holds_alternative<UniqueFd>(result), "real creation after injected failures");
    auto epoll = std::move(std::get<UniqueFd>(result));
    int raw[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, raw) == 0,
        "real socketpair fixture");
    std::array<UniqueFd, 2> sockets{UniqueFd(raw[0]), UniqueFd(raw[1])};
    constexpr auto token = std::numeric_limits<std::uint64_t>::max();
    const auto before_add = control_calls;
    require(!epoll_add(epoll.get(), sockets[0].get(), EPOLLIN | EPOLLRDHUP, token), "real ADD");
    require(control_calls == before_add + 1 && operation == EPOLL_CTL_ADD && !null_event
                && observed.events == (EPOLLIN | EPOLLRDHUP) && observed.data.u64 == token
                && observed_epoll == epoll.get() && observed_fd == sockets[0].get(),
        "ADD copies exact arguments");
    // 每种失败逐个注入ADD/MOD/DEL；计数独立证明未隐藏重试或add/mod切换。
    for (const int failure : {EINTR, ENOMEM, ENOSPC, EBADF, EINVAL, EPERM, EEXIST, ENOENT}) {
        control_error = failure;
        for (const int op : {EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL}) {
            const auto before = control_calls;
            const auto error = op == EPOLL_CTL_ADD ? epoll_add(epoll.get(), raw[1], EPOLLOUT, 0)
                               : op == EPOLL_CTL_MOD
                                   ? epoll_modify(epoll.get(), raw[0], EPOLLOUT, 0)
                                   : epoll_remove(epoll.get(), raw[0]);
            require(error_is(error, failure), "control forwards original errno");
            require(control_calls == before + 1 && operation == op, "one exact operation");
            require(null_event == (op == EPOLL_CTL_DEL), "DEL nullptr only");
            if (op != EPOLL_CTL_DEL)
                require(observed.events == EPOLLOUT && observed.data.u64 == 0,
                    "replacement mask and zero token passed");
            require(::fcntl(epoll.get(), F_GETFD) >= 0 && ::fcntl(raw[0], F_GETFD) >= 0
                        && ::fcntl(raw[1], F_GETFD) >= 0,
                "failed control closes no descriptor");
        }
    }
    control_error = 0;
    // 真实内核仍持原IN/token；包装器失败没有提交更改，不能只断言镜像变量。
    require(::send(raw[1], "x", 1, MSG_DONTWAIT | MSG_NOSIGNAL) == 1, "fixture makes readable");
    epoll_event event{};
    require(::epoll_wait(epoll.get(), &event, 1, 1000) == 1 && (event.events & EPOLLIN) != 0
                && event.data.u64 == token,
        "failed MOD and DEL preserve original kernel registration");
    require(error_is(epoll_modify(epoll.get(), raw[1], EPOLLIN, 1), ENOENT),
        "failed ADD did not create second registration");
    // 所有不支持位逐个拒绝；负fd优先EBADF，均不调用包装器。
    for (const std::uint32_t mask : {static_cast<std::uint32_t>(EPOLLET),
             static_cast<std::uint32_t>(EPOLLONESHOT),
             static_cast<std::uint32_t>(EPOLLEXCLUSIVE),
             static_cast<std::uint32_t>(EPOLLWAKEUP),
             static_cast<std::uint32_t>(EPOLLPRI),
             static_cast<std::uint32_t>(EPOLLERR),
             static_cast<std::uint32_t>(EPOLLHUP),
             std::numeric_limits<std::uint32_t>::max()}) {
        const auto before = control_calls;
        require(error_is(epoll_add(epoll.get(), raw[1], mask, token), EINVAL), "invalid ADD mask");
        require(
            error_is(epoll_modify(epoll.get(), raw[0], mask, token), EINVAL), "invalid MOD mask");
        require(control_calls == before, "invalid masks perform no syscall");
    }
    const auto before = control_calls;
    require(error_is(epoll_add(-1, raw[0], EPOLLET, 0), EBADF), "negative epoll precedence");
    require(error_is(epoll_modify(epoll.get(), -1, EPOLLIN, 0), EBADF), "negative target");
    require(error_is(epoll_remove(epoll.get(), -1), EBADF), "negative DEL");
    require(control_calls == before, "negative arguments perform no syscall");
    require(!epoll_modify(epoll.get(), raw[0], 0, token), "valid zero mask reaches kernel");
    require(operation == EPOLL_CTL_MOD && observed.events == 0 && observed.data.u64 == token,
        "zero mask and full u64 copied");
    require(!epoll_remove(epoll.get(), raw[0]) && null_event && operation == EPOLL_CTL_DEL,
        "real DEL uses nullptr");
    std::cout << "checks=" << checks << " failures=0 test_only_wrap=epoll_create1,epoll_ctl\n";
}
