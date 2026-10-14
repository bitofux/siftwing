/*
 * PROJECT : SIFTWING
 * FILE    : connection_identity_reuse_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 确定性Linux同fd/同slot/同request_id复用后的旧结果拒绝
 */
#include "siftwing/concurrency/routed_payload.hpp"
#include "siftwing/net/connection.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
namespace {
using namespace siftwing::net;
using namespace siftwing::concurrency;
unsigned int checks = 0;
void require(bool v, const char *why) {
    ++checks;
    if (!v) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
void send_request(UniqueFd &peer) {
    auto bytes = std::get<std::string>(
        encode_frame({MessageType::search_request, 0, 77, "{}"}, ReceiveRole::server, 128));
    require(::send(peer.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL)
                == static_cast<ssize_t>(bytes.size()),
        "local complete request sent");
}
RoutedSearchTask request(Connection &connection, ConnectionId identity) {
    auto read = connection.read_request(256, 16);
    require(
        read.stop == ReadStop::request && !read.body_error, "real Connection admits search frame");
    auto source_result = SearchTask::create(*read.request, 128);
    auto source = std::get<SearchTask>(std::move(source_result));
    auto result = RoutedSearchTask::create(identity, source);
    return std::get<RoutedSearchTask>(std::move(result));
}
RoutedCompletion result(const RoutedSearchTask &task, std::string_view body) {
    auto r = RoutedCompletion::success(task, body);
    return std::get<RoutedCompletion>(std::move(r));
}
// 仅测试未来接收者必须遵守的门禁；不是正式Completion回投实现。
bool apply(ConnectionIdentityTable &table, Connection &current, const RoutedCompletion &output) {
    if (!output.valid() || !table.matches(output.route()) || current.get() < 0)
        return false;
    return current.queue_response(*output.payload()->frame()).stop == QueueStop::queued;
}
}
int main() {
    ConnectionIdentityTable table(7, 1);
    for (unsigned int round = 0; round < 64; ++round) {
        int pair[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
            "old socketpair");
        UniqueFd low(pair[0]), old_peer(pair[1]);
        // 为本测试保留一个高数字，再关闭后只复用该确知已释放的位置，不覆盖其他拥有者。
        const int reused = ::fcntl(low.get(), F_DUPFD_CLOEXEC, 160);
        require(reused >= 160, "reserved numeric fd");
        require(!low.close(), "original low alias closed");
        Connection old(UniqueFd(reused), 128);
        const auto old_registration = std::get<ConnectionRegistration>(table.activate(0));
        send_request(old_peer);
        auto old_task = request(old, old_registration.identity);
        auto delayed = result(old_task, "{\"old\":true}");
        require(table.retire(old_registration.identity) && !old.close() && !old_peer.close(),
            "identity invalidated before old fd close");
        require(
            ::fcntl(reused, F_GETFD) == -1 && errno == EBADF, "released fd verified before reuse");
        require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
            "replacement socketpair");
        UniqueFd replacement_source(pair[0]), peer(pair[1]);
        if (replacement_source.get() != reused) {
            require(peer.get() != reused
                        && ::dup3(replacement_source.get(), reused, O_CLOEXEC) == reused,
                "exact numeric fd reuse");
            require(!replacement_source.close(), "replacement low alias closed");
            replacement_source = UniqueFd(reused);
        }
        Connection current(std::move(replacement_source), 128);
        const auto now = std::get<ConnectionRegistration>(table.activate(0));
        require(current.get() == reused
                    && now.identity.slot_index == old_registration.identity.slot_index
                    && now.identity.generation == old_registration.identity.generation + 1,
            "same fd/slot, different generation");
        send_request(peer);
        auto fresh = request(current, now.identity);
        require(fresh.payload()->request()->request_id == delayed.payload()->frame()->request_id,
            "same request id cannot discriminate replacement");
        require(!table.lookup(old_registration.poll_token)
                    && !table.retire(old_registration.identity)
                    && table.lookup(now.poll_token) == now.identity,
            "old event/retirement cannot reach replacement");
        require(!apply(table, current, delayed) && !current.needs_write(),
            "old completion rejected before queueing response");
        char buffer[256];
        require(::recv(peer.get(), buffer, sizeof(buffer), 0) == -1
                    && (errno == EAGAIN || errno == EWOULDBLOCK),
            "stale result produces no peer bytes");
        auto output = result(fresh, "{\"new\":true}");
        auto wrong = output.route();
        ++wrong.reactor_id;
        require(!table.matches(wrong), "cross Sub owner rejected before application");
        wrong = output.route();
        ++wrong.slot_index;
        require(!table.matches(wrong), "unknown slot rejected");
        require(apply(table, current, output), "current route accepted");
        require(
            current.flush_output(256, 16).stop == WriteStop::drained && current.finish_request(),
            "normal Connection response behavior unchanged");
        const auto count = ::recv(peer.get(), buffer, sizeof(buffer), 0);
        const auto expected = std::get<std::string>(encode_frame(
            {MessageType::search_response, 0, 77, "{\"new\":true}"}, ReceiveRole::client, 128));
        require(count > 0 && std::string_view(buffer, static_cast<std::size_t>(count)) == expected,
            "only current response bytes delivered");
        require(table.retire(now.identity) && !current.close() && !peer.close(),
            "replacement resources reclaimed");
    }
    std::cout << "identity reuse checks=" << checks << '\n';
}
