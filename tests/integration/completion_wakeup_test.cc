/*
 * PROJECT : SIFTWING
 * FILE    : completion_wakeup_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 真实Sub应用门禁、半关闭/缓存继续及确定同fd代次复用的独立字节参考
 */
#include "../support/completion_fixture.hpp"
#include <sys/eventfd.h>
namespace {
using namespace siftwing::test;
unsigned int checks = 0;
void check(bool value, const char *why) {
    require(value, why, checks);
}
void empty_peer(int fd) {
    char byte;
    check(::recv(fd, &byte, 1, MSG_DONTWAIT) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
        "rejected completion produces no peer byte");
}
}
int main() {
    const auto baseline = fd_count();
    for (const auto capacity : {0U, 1025U}) {
        for (bool budget : {false, true}) {
            ProtocolSubConfig config;
            (budget ? config.completion_budget : config.completion_capacity) = capacity;
            bool rejected = false;
            try {
                ProtocolSubReactor bad(config);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            check(rejected, "completion bounds checked before resources");
        }
    }
    {
        CompletionQueue queue(7, 2);
        auto wake = std::get<UniqueFd>(create_eventfd_wakeup());
        // eventfd饱和已有可读通知；EAGAIN不能撤销真实队列提交。
        const std::uint64_t saturation = UINT64_MAX - 1;
        check(::write(wake.get(), &saturation, sizeof(saturation)) == 8, "real eventfd saturation");
        auto first = fabricated_task({7, 0, 1});
        publish({&queue, wake.get()}, completion(*first));
        auto second = fabricated_task({7, 1, 1}, 78);
        publish({&queue, wake.get()}, completion(*second));
        auto full = completion(*first);
        const auto *pointer = full.get();
        const auto refused = try_publish_and_notify(queue, full, -1);
        check(refused.publish == CompletionPublishResult::full && !refused.notification_error
                  && full.get() == pointer,
            "full retains source and does not notify invalid fd");
        const auto drained = drain_eventfd(wake.get(), 1);
        check(drained.state == EventFdDrainState::budget_exhausted && drained.successful_reads == 1,
            "one aggregate read is independent of two results");
        check(queue.try_pop()->payload()->frame()->request_id == 77
                  && queue.try_pop()->payload()->frame()->request_id == 78,
            "real queue FIFO after merged notification");
        check(drain_eventfd(wake.get()).state == EventFdDrainState::drained,
            "aggregate notification cleared");
    }
    {
        CaptureSubmitter sink;
        ProtocolSubConfig config;
        config.reactor_id = 7;
        config.request_submitter = &sink;
        config.read_byte_budget = 7;
        config.write_byte_budget = 5;
        config.io_attempt_budget = 1;
        config.completion_budget = 1;
        config.completion_capacity = 16;
        ProtocolSubReactor sub(config);
        check(std::holds_alternative<std::error_code>(sub.completion_target()),
            "no completion target before ready");
        check(!sub.start(), "async Sub ready");
        auto target = std::get<CompletionTarget>(sub.completion_target());
        check(target.wakeup_fd == std::get<MainReactorTarget>(sub.target()).wakeup_fd,
            "fd handoff and results share one notification fd");
        auto peer = attach(sub);
        send_all(peer.get(), wire(3, 77) + wire(1, 78) + wire(4, 79));
        check(::shutdown(peer.get(), SHUT_WR) == 0, "request half-close keeps response direction");
        auto request = take_request(sink);
        check(request->payload()->request()->request_id == 77 && request->route().reactor_id == 7,
            "Sub submits actual owned request and route");
        auto wrong_id = fabricated_task(request->route(), 99);
        publish(target, completion(*wrong_id));
        auto wrong_type = fabricated_task(request->route(), 77, MessageType::suggest_request);
        publish(target, completion(*wrong_type));
        await([&] {
            return sub.status().completion_rejected == 2;
        });
        empty_peer(peer.get());
        // 重复结果紧随正确结果；不解除门禁、不发送第二份响应。
        publish(target, completion(*request));
        publish(target, completion(*request));
        const auto first_expected = wire(0x8003, 77, "{\"test\":true}") + wire(0x8001, 78);
        check(receive(peer.get(), first_expected.size()) == first_expected,
            "partial output then cached PING use independent golden wire");
        auto suggestion = take_request(sink);
        check(suggestion->payload()->request()->message_type == MessageType::suggest_request
                  && suggestion->payload()->request()->request_id == 79,
            "cached suggest continues after finish");
        publish(target, completion(*suggestion, "{\"suggest\":true}"));
        const auto expected = wire(0x8004, 79, "{\"suggest\":true}");
        check(receive(peer.get(), expected.size()) == expected,
            "suggest result returns on owning Sub");
        await([&] {
            return sub.status().responses == 3 && sub.status().active == 0;
        });
        const auto report = sub.status();
        check(report.completion_received == 5 && report.completion_applied == 2
                  && report.completion_rejected == 3 && !report.first_error,
            "wrong/duplicate results preserve current request");
        stop_sub(sub);
        check(std::holds_alternative<std::error_code>(sub.completion_target()),
            "released completion target unavailable");
    }
    {
        CaptureSubmitter sink;
        ProtocolSubConfig config;
        config.reactor_id = 31;
        config.request_submitter = &sink;
        config.max_connections = 1;
        config.completion_budget = 1;
        ProtocolSubReactor sub(config);
        check(!sub.start(), "reuse Sub ready");
        const auto target = std::get<CompletionTarget>(sub.completion_target());
        UniqueFd placeholder(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        const int reserved = ::fcntl(placeholder.get(), F_DUPFD_CLOEXEC, 160);
        check(reserved >= 160, "reserve high fd outside existing resources");
        UniqueFd reservation(reserved);
        check(!reservation.close(), "release exact reserved position");
        for (unsigned int round = 0; round < 16; ++round) {
            auto old_peer = attach(sub, reserved);
            send_all(old_peer.get(), wire(3, 77));
            auto old = take_request(sink);
            check(!old_peer.close(), "old pending client fully closes");
            await([&] {
                // take_request可早于本轮报告发布；旧active=0不能证明刚注册的fd已关闭。
                return sub.status().active == 0 && ::fcntl(reserved, F_GETFD) == -1
                       && errno == EBADF;
            });
            check(::fcntl(reserved, F_GETFD) == -1 && errno == EBADF,
                "old registered fd is actually released");
            auto peer = attach(sub, reserved);
            send_all(peer.get(), wire(3, 77));
            auto current = take_request(sink);
            check(current->route().slot_index == old->route().slot_index
                      && current->route().generation == old->route().generation + 1,
                "same slot fd and request id have new generation");
            publish(target, completion(*old, "{\"old\":true}"));
            await([&] {
                return sub.status().completion_stale == 2 * round + 1;
            });
            empty_peer(peer.get());
            auto unknown = current->route();
            ++unknown.slot_index;
            publish(target, completion(*fabricated_task(unknown), "{\"unknown\":true}"));
            await([&] {
                return sub.status().completion_stale == 2 * (round + 1);
            });
            publish(target, completion(*current, "{\"new\":true}"));
            const auto expected = wire(0x8003, 77, "{\"new\":true}");
            check(receive(peer.get(), expected.size()) == expected,
                "only new generation writes replacement");
            check(!peer.close(), "replacement peer closes");
            await([&] {
                return sub.status().active == 0 && ::fcntl(reserved, F_GETFD) == -1
                       && errno == EBADF;
            });
        }
        const auto report = sub.status();
        check(report.completion_received == 48 && report.completion_stale == 32
                  && report.completion_applied == 16,
            "generation oracle independent of arrival order");
        stop_sub(sub);
    }
    for (const auto refusal : {TaskSubmitResult::full, TaskSubmitResult::closed}) {
        CaptureSubmitter sink;
        sink.refusal = refusal;
        ProtocolSubConfig config;
        config.request_submitter = &sink;
        config.max_body_bytes = 128;
        ProtocolSubReactor sub(config);
        check(!sub.start(), "refusal Sub ready");
        auto peer = attach(sub);
        send_all(peer.get(), wire(3, 77));
        const auto expected = refusal == TaskSubmitResult::full
                                  ? wire(0xffff, 77, "{\"message\":\"overloaded\"}", 2)
                                  : wire(0xffff, 77, "{\"message\":\"stopping\"}", 3);
        check(receive(peer.get(), expected.size()) == expected,
            "submission refusal has bounded safe error");
        stop_sub(sub);
    }
    check(fd_count() == baseline, "all ready/reuse/refusal resources reclaimed");
    std::cout << "completion wakeup checks=" << checks << '\n';
}
