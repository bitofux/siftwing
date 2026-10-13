/*
 * PROJECT : SIFTWING
 * FILE    : protocol_loop.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : LT有限调度、缓存继续、单调token和逐层错误清理
 */
#include "siftwing/net/protocol_loop.hpp"
#include "siftwing/net/epoll_ops.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <new>
#include <utility>

namespace siftwing::net {
namespace {
std::error_code code(int value) noexcept {
    return {value, std::generic_category()};
}
bool valid(const ProtocolLoopConfig &c) noexcept {
    const auto max_io = frame_header_bytes + frame_max_body_bytes;
    return c.max_connections >= 1 && c.max_connections <= 1024 && c.max_body_bytes >= 2
           && c.max_body_bytes <= frame_max_body_bytes && c.accept_attempt_budget >= 1
           && c.accept_attempt_budget <= 1024 && c.read_byte_budget >= 1
           && c.read_byte_budget <= max_io && c.write_byte_budget >= 1
           && c.write_byte_budget <= max_io && c.io_attempt_budget >= 1
           && c.io_attempt_budget <= 1024;
}
void first_error(std::error_code &first, std::error_code error) noexcept {
    if (!first && error)
        first = error;
}
} // namespace
ProtocolLoop::ProtocolLoop(std::vector<Slot> &&slots,
    Listener &&listener,
    UniqueFd &&epoll,
    const ProtocolLoopConfig &config) noexcept
    : slots_(std::move(slots)), listener_(std::move(listener)), epoll_(std::move(epoll)),
      config_(config) {}
ProtocolLoop::~ProtocolLoop() {
    (void)close();
}

std::variant<std::unique_ptr<ProtocolLoop>, std::error_code> ProtocolLoop::create(
    const ListenConfig &listen, const ProtocolLoopConfig &config) {
    if (!valid(config))
        return code(EINVAL);
    std::vector<Slot> slots(config.max_connections);
    auto listening = Listener::create(listen);
    if (auto *error = std::get_if<std::error_code>(&listening))
        return *error;
    auto poller = create_epoll();
    if (auto *error = std::get_if<std::error_code>(&poller))
        return *error;
    auto &listener = std::get<Listener>(listening);
    auto &epoll = std::get<UniqueFd>(poller);
    if (const auto error = epoll_add(epoll.get(), listener.get(), EPOLLIN, 0))
        return error;
    return std::unique_ptr<ProtocolLoop>(
        new ProtocolLoop(std::move(slots), std::move(listener), std::move(epoll), config));
}
std::error_code ProtocolLoop::close() noexcept {
    std::error_code first;
    for (auto &slot : slots_) {
        if (slot.connection) {
            // Connection可能已自行终止；不对已关闭、可能被复用的旧数字执行DEL。
            if (slot.connection->get() >= 0) {
                first_error(first, epoll_remove(epoll_.get(), slot.connection->get()));
                first_error(first, slot.connection->close());
            }
            slot.connection.reset();
        }
        slot.token = 0;
        slot.mask = 0;
        slot.runnable = false;
        slot.serviced = false;
    }
    if (listener_.get() >= 0) {
        first_error(first, epoll_remove(epoll_.get(), listener_.get()));
        first_error(first, listener_.close());
    }
    first_error(first, epoll_.close());
    active_ = 0;
    listener_paused_ = false;
    return first;
}
void ProtocolLoop::fail(LoopStep &result, std::error_code error) noexcept {
    result.dropped_connections += active_;
    (void)close();
    terminal_error_ = error;
    result.error = error;
    result.stop = LoopStop::failed;
}
bool ProtocolLoop::refresh_listener(LoopStep &result) noexcept {
    const bool pause = active_ == slots_.size();
    if (pause == listener_paused_)
        return true;
    if (const auto error = epoll_modify(epoll_.get(), listener_.get(), pause ? 0U : EPOLLIN, 0)) {
        fail(result, error);
        return false;
    }
    listener_paused_ = pause;
    return true;
}
bool ProtocolLoop::drop(std::size_t index, LoopStep &result, std::error_code error) noexcept {
    auto &slot = slots_[index];
    first_error(result.first_connection_error, error);
    if (slot.connection->get() >= 0) {
        first_error(
            result.first_connection_error, epoll_remove(epoll_.get(), slot.connection->get()));
        first_error(result.first_connection_error, slot.connection->close());
    }
    slot.connection.reset();
    slot.token = 0;
    slot.mask = 0;
    slot.runnable = false;
    --active_;
    ++result.dropped_connections;
    return refresh_listener(result);
}
bool ProtocolLoop::accept_ready(LoopStep &result) noexcept {
    if (active_ == slots_.size())
        return refresh_listener(result);
    std::array<UniqueFd, 16> incoming{};
    const auto batch = listener_.accept_into(incoming.data(),
        std::min(incoming.size(), slots_.size() - active_),
        config_.accept_attempt_budget);
    result.accept_attempts += batch.attempts;
    result.accepted += batch.accepted;
    if (batch.stop == AcceptStop::error) {
        // 成功前缀仍由本地incoming拥有；失败终止后随作用域关闭，不留下资源耗尽忙循环。
        result.rejected_connections += batch.accepted;
        fail(result, batch.error);
        return false;
    }
    for (std::size_t i = 0; i < batch.accepted; ++i) {
        if (next_token_ == 0) {
            result.rejected_connections += batch.accepted - i;
            fail(result, code(EOVERFLOW));
            return false;
        }
        try {
            auto connection =
                std::make_unique<Connection>(std::move(incoming[i]), config_.max_body_bytes);
            const auto token = next_token_;
            next_token_ = token == std::numeric_limits<std::uint64_t>::max() ? 0 : token + 1;
            const std::uint32_t mask = EPOLLIN | EPOLLRDHUP;
            if (const auto error = epoll_add(epoll_.get(), connection->get(), mask, token)) {
                ++result.rejected_connections;
                first_error(result.first_connection_error, error);
                continue;
            }
            auto slot = std::find_if(slots_.begin(), slots_.end(), [](const Slot &s) {
                return !s.connection;
            });
            slot->connection = std::move(connection);
            slot->token = token;
            slot->mask = mask;
            slot->runnable = false;
            slot->serviced = false;
            ++active_;
        } catch (const std::bad_alloc &) {
            result.rejected_connections += batch.accepted - i;
            fail(result, code(ENOMEM));
            return false;
        } catch (...) {
            result.rejected_connections += batch.accepted - i;
            fail(result, code(EIO));
            return false;
        }
    }
    return refresh_listener(result);
}
bool ProtocolLoop::service(std::size_t index, LoopStep &result) noexcept {
    auto &slot = slots_[index];
    auto &connection = *slot.connection;
    try {
        if (!connection.request_pending()) {
            auto read =
                connection.read_request(config_.read_byte_budget, config_.io_attempt_budget);
            result.read_bytes += read.bytes;
            result.read_attempts += read.attempts;
            if (read.stop == ReadStop::request) {
                ++result.requests;
                // 只接通协议I/O；未定义业务响应字段，不把占位{}伪装成搜索成功。
                auto response = !read.body_error && read.request->message_type == MessageType::ping
                                    ? make_response(*read.request, "{}", config_.max_body_bytes)
                                    : make_error_response(*read.request,
                                          read.body_error == ProtocolError::json_depth
                                              ? ErrorStatus::resource_limit
                                              : ErrorStatus::invalid_request,
                                          config_.max_body_bytes);
                auto *frame = std::get_if<Frame>(&response);
                if (!frame || connection.queue_response(*frame).stop != QueueStop::queued)
                    return drop(index, result, code(EPROTO));
            } else if (read.stop == ReadStop::budget_exhausted) {
                slot.runnable = true;
            } else if (read.stop != ReadStop::would_block) {
                return drop(index, result, read.error);
            }
        }
        if (connection.needs_write()) {
            const auto write =
                connection.flush_output(config_.write_byte_budget, config_.io_attempt_budget);
            result.write_bytes += write.bytes;
            result.write_attempts += write.attempts;
            if (write.stop == WriteStop::drained) {
                if (!connection.finish_request())
                    return drop(index, result, code(EIO));
                ++result.completed_responses;
                slot.runnable = connection.buffered_bytes() != 0;
            } else if (write.stop != WriteStop::would_block
                       && write.stop != WriteStop::budget_exhausted) {
                return drop(index, result, write.error);
            }
        }
        // 暂停读期间不订阅RDHUP；FIN就绪是持续的，否则慢写会因半关闭空转。
        const std::uint32_t mask = connection.needs_write() ? EPOLLOUT : (EPOLLIN | EPOLLRDHUP);
        if (mask != slot.mask) {
            if (const auto error = epoll_modify(epoll_.get(), connection.get(), mask, slot.token))
                return drop(index, result, error);
            slot.mask = mask;
        }
        return true;
    } catch (const std::bad_alloc &) {
        return drop(index, result, code(ENOMEM));
    } catch (...) {
        return drop(index, result, code(EIO));
    }
}
LoopStep ProtocolLoop::run_once(int timeout_ms) noexcept {
    LoopStep result;
    if (!epoll_) {
        result.stop = terminal_error_ ? LoopStop::failed : LoopStop::closed;
        result.error = terminal_error_;
        return result;
    }
    if (timeout_ms < -1) {
        result.stop = LoopStop::invalid_timeout;
        result.error = code(EINVAL);
        return result;
    }
    for (auto &slot : slots_)
        slot.serviced = false;
    bool scheduled = false;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto &slot = slots_[i];
        if (slot.connection && slot.runnable) {
            scheduled = true;
            slot.runnable = false;
            slot.serviced = true;
            if (!service(i, result))
                return result;
        }
    }
    std::array<epoll_event, 64> events{};
    const int count = ::epoll_wait(
        epoll_.get(), events.data(), static_cast<int>(events.size()), scheduled ? 0 : timeout_ms);
    if (count < 0) {
        const auto error = code(errno);
        if (error.value() == EINTR) {
            result.stop = LoopStop::interrupted;
            result.error = error;
        } else
            fail(result, error);
        return result;
    }
    if (count > static_cast<int>(events.size())) {
        fail(result, code(EIO));
        return result;
    }
    result.events = static_cast<std::size_t>(count);
    if (count == 0) {
        result.stop = scheduled ? LoopStop::progress : LoopStop::idle;
        return result;
    }
    bool listener_serviced = false;
    for (int i = 0; i < count; ++i) {
        const auto token = events[static_cast<std::size_t>(i)].data.u64;
        if (token == 0) {
            if (!listener_serviced) {
                listener_serviced = true;
                if (!accept_ready(result))
                    return result;
            }
            continue;
        }
        const auto slot = std::find_if(slots_.begin(), slots_.end(), [token](const Slot &s) {
            return s.connection && s.token == token;
        });
        // 已取到的批次不会因DEL被清空；只派发仍存活的身份，且每轮至多服务一次。
        if (slot == slots_.end() || slot->serviced)
            continue;
        slot->serviced = true;
        slot->runnable = false;
        if (!service(static_cast<std::size_t>(slot - slots_.begin()), result))
            return result;
    }
    return result;
}
} // namespace siftwing::net
