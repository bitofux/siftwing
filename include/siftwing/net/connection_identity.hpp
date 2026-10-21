/*
 * PROJECT : SIFTWING
 * FILE    : connection_identity.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 与fd数字分离的路由身份、固定slot代次及单调epoll token桥接
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace siftwing::net {
/**
 * @brief 可复制的纯值路由身份，无Connection/fd借用；generation为0表示无效
 * @note reactor_id/slot_index为本地命名空间，0都是合法值，不是线上协议字段。
 *     控制者保证同时可回投的不同Reactor使用不同id；旧结果可能存在时不能重用其命名空间。
 *     数值合法不证明连接仍在、请求仍pending或期限未过，须在所属I/O线程查表。
 */
struct ConnectionId final {
    std::uint32_t reactor_id = 0; ///< 固定所属Reactor编号；由控制者分配而非fd/线程id推导。
    std::uint32_t slot_index = 0; ///< 固定有界slot下标；关闭后可复用，须连同generation比较。
    std::uint64_t generation = 0; ///< 同slot单调非零代次；关闭保留历史，不回绕。
    /** @brief 仅判断非零代次，不能替代身份表的存活校验。 */
    [[nodiscard]] bool valid() const noexcept {
        return generation != 0;
    }
    /** @brief 比较全部字段的数值相等；两个无效值也可能相等，不证明存活。 */
    friend bool operator==(ConnectionId a, ConnectionId b) noexcept {
        return a.reactor_id == b.reactor_id && a.slot_index == b.slot_index
               && a.generation == b.generation;
    }
    /** @brief 全字段相等的逻辑否定，无连接访问。 */
    friend bool operator!=(ConnectionId a, ConnectionId b) noexcept {
        return !(a == b);
    }
};
/** @brief 本地身份建立失败，不转成线上协议状态；四种拒绝均不改变表。 */
enum class IdentityError {
    invalid_slot,
    already_live,
    generation_exhausted,
    token_exhausted
};
/** @brief 完整身份与当前epoll绑定；两者都是值，不能用poll_token直接回投业务结果。 */
struct ConnectionRegistration final {
    ConnectionId identity;    ///< 固定所属/slot/代次，可进入跨线程拥有载荷。
    std::uint64_t poll_token; ///< 仅本表的单调注册key；0保留给Reactor控制事件。
};
/**
 * @brief 单个Reactor独占的固定身份表；不拥有Connection/socket，不自动执行ADD/DEL/close
 * @note 构造/查询/修改/析构仅由所属线程串行调用；const不提供同步，不复制/移动。
 *     activate先建立本地绑定，调用者随后ADD并发布Connection；ADD失败须retire。
 *     retire在关闭/释放slot前执行。查表通过只说明表中的当前注册，实际Connection状态须另核。
 *     表重建会重置代次/token，旧结果/旧epoll批次可能存在时不得重用reactor命名空间/本表。
 */
class ConnectionIdentityTable final {
public:
    /**
     * @brief 验证[1,65536]slot容量后预分配，所有slot初始无效；reactor_id允许任意uint32
     * @param[in] reactor_id
     *     本表固定命名空间；跨表唯一性与重建生命周期由外部控制者保证。
     * @param[in] slots
     *     固定容量[1,65536]，不是活跃fd/RSS或全局连接上限。
     *
     * @throws
     *     非法容量invalid_argument，预分配bad_alloc/length_error；不发布半构造对象。
     */
    explicit ConnectionIdentityTable(std::uint32_t reactor_id, std::size_t slots);
    /** @brief 释放表存储；不关闭任何fd，调用者先清理Connection且撤销借用。 */
    ~ConnectionIdentityTable() noexcept = default;
    ConnectionIdentityTable(const ConnectionIdentityTable &) = delete;
    ConnectionIdentityTable &operator=(const ConnectionIdentityTable &) = delete;
    ConnectionIdentityTable(ConnectionIdentityTable &&) = delete;
    ConnectionIdentityTable &operator=(ConnectionIdentityTable &&) = delete;
    /**
     * @brief 在空slot建立下一代身份与单调token，不分配、不抛异常
     * @param[in] slot_index
     *     小于capacity的slot；当前有注册时already_live，不隐式替换。
     *
     * @return
     *     拥有ConnectionRegistration或IdentityError，按invalid_slot→already_live→generation→token检查。
     * @note 每个成功激活消费一个generation/token，包括后来ADD失败而retire的尝试。
     *     uint64最大值可使用一次，耗尽后拒绝，不回绕或重用旧身份。
     */
    [[nodiscard]] std::variant<ConnectionRegistration, IdentityError> activate(
        std::size_t slot_index) noexcept;
    /** @brief 精确当前身份才撤销绑定；旧/外表/无效身份false且保持新注册，重复false。 */
    [[nodiscard]] bool retire(ConnectionId identity) noexcept;
    /** @brief 所属线程核全字段、范围与当前非零注册；不判断fd打开、request_id或deadline。 */
    [[nodiscard]] bool matches(ConnectionId identity) const noexcept;
    /** @brief 按当前非零epoll token取拥有身份；0、陈旧或未知token返回nullopt，无指针借用。 */
    [[nodiscard]] std::optional<ConnectionId> lookup(std::uint64_t poll_token) const noexcept;
    /** @brief 固定slot容量的按值观测，不提供跨线程同步。 */
    [[nodiscard]] std::size_t capacity() const noexcept {
        return entries_.size();
    }

private:
    struct Entry {
        std::uint64_t generation = 0; ///< 最近成功激活代次，retire不重置。
        std::uint64_t token = 0;      ///< 当前注册key；0表示无活跃绑定。
    };
    const std::uint32_t reactor_id_; ///< 构造冻结，查询比较所属命名空间。
    std::vector<Entry> entries_; ///< 固定容量元数据，不拥有Connection；操作不扩容。
    std::uint64_t next_token_ = 1; ///< 0表示全表token耗尽；当前最大token仍可查询/撤销。
};
} // namespace siftwing::net
