/*
 * PROJECT : SIFTWING
 * FILE    : listener.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : Linux TCP监听拥有者、原子accept4和有界接入合同
 */
#pragma once

#include "siftwing/net/socket_ops.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <variant>

namespace siftwing::net {
/** @brief 同步借用数值IP地址；不解析DNS、IPv6 zone或隐式双栈。 */
struct ListenConfig final {
    IpFamily family = IpFamily::v4; ///< 地址族须与address匹配，IPv6强制V6ONLY。
    std::string_view address = "127.0.0.1"; ///< 创建期间有效的数值地址；空值、NUL和不匹配拒绝。
    std::uint16_t port = 0; ///< 主机字节序；0请求内核选择端口。
    int backlog = 128; ///< 正的listen队列请求，实际值可能被内核somaxconn截断。
    bool reuse_address = false; ///< 显式SO_REUSEADDR；不设置SO_REUSEPORT或自动共享监听者。
};
/** @brief 批次停止原因；满槽/预算耗尽不证明队列为空，LT下须后续继续调度。 */
enum class AcceptStop {
    drained,
    output_full,
    budget_exhausted,
    error
};
/** @brief 一次批次的统计；已成功fd在调用方槽位里，错误不回收此前成功结果。 */
struct AcceptBatch final {
    std::size_t attempts = 0; ///< 实际accept4次数；成功、EINTR及网络错误均计入预算。
    std::size_t accepted = 0; ///< destination[0,accepted)独占新fd，其余槽位保持。
    AcceptStop stop = AcceptStop::error; ///< drained只来自EAGAIN/EWOULDBLOCK。
    std::error_code error{}; ///< drained保存EAGAIN类错误，error保存原errno，另外两种为空。
};
/**
 * @brief 独占一个非阻塞TCP监听socket；不拥有已交付连接、不存储epoll注册
 *
 * @note 对象/借用fd/接入槽位归属一个I/O线程或由调用方同步；const不保证并发安全。
 *     get()只供借用，不得外部close、改变NONBLOCK或为同一数字创建第二个拥有者。
 *     无回调、动态分配、业务读取或Sub Reactor交接；errno不保证保持（close沿用UniqueFd）。
 */
class Listener final {
public:
    /** @brief 构造空监听者；接入返回EBADF。 */
    Listener() noexcept = default;
    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;
    /** @brief 转交监听资源，源为空；已有接入fd不随之移动或关闭。 */
    Listener(Listener &&) noexcept = default;
    /** @brief 关闭旧监听资源一次后接管源；自移动由UniqueFd保持。 */
    Listener &operator=(Listener &&) noexcept = default;
    /**
     * @brief 校验数值地址，创建NONBLOCK/CLOEXEC socket，再配置、bind、listen和查询端口
     *
     * @param[in] config
     *     创建期间稳定的配置；不保存address借用。IPv6固定V6ONLY，reuse_address显式选择。
     *
     * @return
     *     完整监听者或generic_category原errno；预检失败EINVAL且不创建socket。
     *     系统步骤失败不重试，包括EINTR；已创建资源由UniqueFd关闭一次，无部分监听者。
     *
     * @note 本接口无fcntl退化；端口为0时查询实际端口。backlog不是连接数/应用队列硬限。
     */
    [[nodiscard]] static std::variant<Listener, std::error_code> create(
        const ListenConfig &config) noexcept;
    /** @brief 借用监听fd，空时-1；移动、close、析构后失效。 */
    [[nodiscard]] int get() const & noexcept {
        return socket_.get();
    }
    int get() const && = delete;
    /** @brief 内核实际本地端口，主机字节序；空或移出对象为0。 */
    [[nodiscard]] std::uint16_t port() const noexcept {
        return socket_ ? port_ : 0;
    }
    /** @brief 关闭监听资源一次，不关闭已交付连接；epoll注销仍由调用方负责。 */
    [[nodiscard]] std::error_code close() noexcept {
        return socket_.close();
    }
    /**
     * @brief 单次accept4(nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC)
     *
     * @return
     *     独占新连接fd（0合法）或原errno；空监听者EBADF且无syscall。包括EINTR，不隐式重试。
     *     不订阅epoll、不获取peer元数据、不读业务、不改变监听者所有权。
     */
    [[nodiscard]] SocketResult accept_one() const noexcept;
    /**
     * @brief 向调用方空槽位接入，按实际accept4尝试次数限制一批工作
     *
     * @param[out] destination
     *     至少capacity个已构造、可写且不与监听拥有权重叠的UniqueFd；须在调用期间稳定。
     *     前min(capacity,attempt_budget)个槽位须为空，预检不满足则EINVAL且全部保持。
     * @param[in] capacity
     *     正槽位数；结果仅填充成功前缀，不覆盖其余槽位。
     * @param[in] attempt_budget
     *     正尝试次数，含EINTR/可重试网络错误；不代表CPU时间或全局连接限额。
     *
     * @return
     *     已接入数、实际尝试数和停止原因；错误保留成功前缀及监听fd。
     *     EINTR、ECONNABORTED和Linux文档列出的pending network errors按预算继续；
     *     EAGAIN/EWOULDBLOCK为drained。其他错误（含EMFILE/ENFILE/ENOBUFS/ENOMEM）立即停止。
     *     最后一次成功同时填满槽位并耗尽预算时优先output_full；二者均不承诺队列为空。
     *
     * @note 空监听者EBADF优先；其后检查指针/正预算/空槽，均不调用accept4。
     *     自身无分配/异常；耗尽资源后的退避、暂停监听和再次调度由后续上层决定，不能忙重试。
     */
    [[nodiscard]] AcceptBatch accept_into(
        UniqueFd *destination, std::size_t capacity, std::size_t attempt_budget) const noexcept;

private:
    UniqueFd socket_{}; ///< 唯一监听拥有者；析构沿用UniqueFd的Linux单次close合同。
    std::uint16_t port_ = 0; ///< 成功getsockname产生；仅socket_有效时对外可读。
};
using ListenerResult = std::variant<Listener, std::error_code>; ///< 完整监听者或创建错误。
} // namespace siftwing::net
