/*
 * PROJECT : SIFTWING
 * FILE    : connection_identity.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 激活前拒绝、关闭保留代次、全字段校验和单调token转换
 */
#include "siftwing/net/connection_identity.hpp"
#include <limits>
#include <stdexcept>
namespace siftwing::net {
namespace {
std::size_t checked(std::size_t slots) {
    if (slots == 0 || slots > 1024)
        throw std::invalid_argument("invalid identity table capacity");
    return slots;
}
}
ConnectionIdentityTable::ConnectionIdentityTable(std::uint32_t reactor, std::size_t slots)
    : reactor_id_(reactor), entries_(checked(slots)) {}
std::variant<ConnectionRegistration, IdentityError> ConnectionIdentityTable::activate(
    std::size_t slot) noexcept {
    if (slot >= entries_.size())
        return IdentityError::invalid_slot;
    auto &entry = entries_[slot];
    if (entry.token != 0)
        return IdentityError::already_live;
    if (entry.generation == std::numeric_limits<std::uint64_t>::max())
        return IdentityError::generation_exhausted;
    if (next_token_ == 0)
        return IdentityError::token_exhausted;
    // 所有拒绝在提交之前；预分配Entry及纯值结果保证提交之后不抛。
    ++entry.generation;
    entry.token = next_token_;
    next_token_ = next_token_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_token_ + 1;
    return ConnectionRegistration{
        {reactor_id_, static_cast<std::uint32_t>(slot), entry.generation}, entry.token};
}
bool ConnectionIdentityTable::matches(ConnectionId id) const noexcept {
    return id.valid() && id.reactor_id == reactor_id_ && id.slot_index < entries_.size()
           && entries_[id.slot_index].token != 0
           && entries_[id.slot_index].generation == id.generation;
}
bool ConnectionIdentityTable::retire(ConnectionId id) noexcept {
    if (!matches(id))
        return false;
    entries_[id.slot_index].token = 0;
    return true;
}
std::optional<ConnectionId> ConnectionIdentityTable::lookup(std::uint64_t token) const noexcept {
    if (token == 0)
        return std::nullopt;
    for (std::size_t slot = 0; slot < entries_.size(); ++slot)
        if (entries_[slot].token == token)
            return ConnectionId{
                reactor_id_, static_cast<std::uint32_t>(slot), entries_[slot].generation};
    return std::nullopt;
}
} // namespace siftwing::net
