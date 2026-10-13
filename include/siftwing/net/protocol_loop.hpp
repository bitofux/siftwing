/*
 * PROJECT : SIFTWING
 * FILE    : protocol_loop.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 所属单线程的LT协议I/O基线、有限调度和独占资源合同
 */
#pragma once

#include "siftwing/net/connection.hpp"
#include "siftwing/net/listener.hpp"
#include <memory>
#include <vector>

namespace siftwing::net {
/** @brief 固定启动配置；预算限制I/O次数/字节，不限制JSON CPU或进程RSS。 */
struct ProtocolLoopConfig final {
    std::size_t max_connections = 128; ///< [1,1024]，固定slot；满时暂停监听IN。
    std::size_t max_body_bytes =
        frame_max_body_bytes;               ///< [2,frame_max_body_bytes]，每Buffer独立限额。
    std::size_t accept_attempt_budget = 16; ///< [1,1024]，每轮接入尝试，含EINTR；最多16个新fd。
    std::size_t read_byte_budget = 65536; ///< [1,24+frame_max_body_bytes]，每连接每轮recv预算。
    std::size_t write_byte_budget = 65536; ///< 同上，每连接每轮send预算。
    std::size_t io_attempt_budget = 16; ///< [1,1024]，每连接read和flush各自的尝试预算。
};
/** @brief 一次调度结束原因；failed为Loop终止，连接局部错误不使Loop终止。 */
enum class LoopStop {
    progress,        ///< 本轮调度完成；可能仅忽略陈旧事件。
    idle,            ///< 无缓存工作且epoll_wait返回0。
    interrupted,     ///< epoll_wait的EINTR，无隐藏重试，Loop保持。
    invalid_timeout, ///< 小于-1的timeout，EINVAL，无wait且状态保持。
    failed,          ///< Loop已因系统/接入资源错误终止，error保存首因。
    closed           ///< 显式终止后查询，无系统调用。
};
/** @brief 每轮统计，终止后重复查询计数为0；完成响应仅表示交给内核。 */
struct LoopStep final {
    LoopStop stop = LoopStop::progress; ///< progress可能仅处理陈旧事件，无业务完成承诺。
    std::size_t events = 0;             ///< 本次epoll_wait实际返回数，最多64。
    std::size_t accept_attempts = 0; ///< accept4尝试数，包含失败和EINTR。
    std::size_t accepted = 0;        ///< 内核交付的新fd数，包括后来未被注册的fd。
    std::size_t rejected_connections = 0; ///< 已接入但未被注册的fd，均关闭。
    std::size_t dropped_connections = 0; ///< 本轮释放的已注册连接，包括Loop失败时全部连接。
    std::size_t requests = 0; ///< 完整受信请求数，包括Body拒绝和未接业务的类型。
    std::size_t completed_responses = 0; ///< 输出排空且显式finish成功数。
    std::size_t read_bytes =
        0; ///< 已返回ReadResult的recv字节，不计缓存；读取异常未返回的计数不计入。
    std::size_t write_bytes = 0; ///< 本轮实际send字节，不证明对端接收。
    std::size_t read_attempts =
        0; ///< 已返回ReadResult的recv尝试，含EINTR/EAGAIN/EOF；读取异常未返回计数不计入。
    std::size_t write_attempts = 0; ///< 包含EINTR/EAGAIN的send尝试。
    std::error_code error; ///< failed首因、interrupted的EINTR或invalid_timeout的EINVAL。
    std::error_code
        first_connection_error; ///< 首个局部系统/本地错误；EOF/已响应的Body拒绝不占此字段。
};

/**
 * @brief 独占一个Listener、epoll及固定有界连接slot，不创建线程
 *
 * @note 所有API与析构仅由所属I/O线程串行调用；不复制/移动、不外露client fd。
 *     只处理PING/PONG及受信错误；其他合法请求返回INVALID_REQUEST，尚未执行搜索业务。
 *     listener token为0，连接token单调不复用，按存活token派发；不依赖复用fd数字。
 */
class ProtocolLoop final {
public:
    /**
     * @brief 验证配置、分配slot、创建监听和epoll、注册监听，成功完整交付拥有者
     *
     * @param[in] listen
     *     数值地址借用仅在创建期间有效，合同同Listener::create。
     * @param[in] config
     *     固定配置；所有字段符合上述范围。
     *
     * @return
     *     独占Loop或原系统errno；非法配置EINVAL且无系统调用。任何失败不留下监听/epoll。
     *
     * @throws
     *     slot/Loop对象分配异常传播；已创建资源由RAII释放，不发布部分对象。
     */
    [[nodiscard]] static std::variant<std::unique_ptr<ProtocolLoop>, std::error_code> create(
        const ListenConfig &listen = {}, const ProtocolLoopConfig &config = {});
    /** @brief 析构立即清理全部资源，沿用close的单次关闭规则，不抛异常。 */
    ~ProtocolLoop();
    ProtocolLoop(const ProtocolLoop &) = delete;
    ProtocolLoop &operator=(const ProtocolLoop &) = delete;
    ProtocolLoop(ProtocolLoop &&) = delete;
    ProtocolLoop &operator=(ProtocolLoop &&) = delete;
    /** @brief 内核实际监听端口；终止后0，无fd借用或异步身份承诺。 */
    [[nodiscard]] std::uint16_t port() const noexcept {
        return listener_.port();
    }
    /** @brief 当前已注册连接数，const不提供并发同步。 */
    [[nodiscard]] std::size_t connection_count() const noexcept {
        return active_;
    }
    /** @brief 是否因容量暂停普通监听IN；终止后false。 */
    [[nodiscard]] bool listener_paused() const noexcept {
        return listener_paused_;
    }
    /**
     * @brief 先服务用户缓存调度，再执行一次epoll_wait并派发有限事件
     *
     * @param[in] timeout_ms
     *     [-1,INT_MAX]，-1无限等待；存在缓存工作时本次实际wait用0。
     *
     * @return
     *     终止优先、非法timeout保持且无wait；EINTR不重试。每连接每轮至多服务一次、交付
     *     一个请求，read/flush各受双预算限制。预算/缓存待办留到下轮，不依赖新EPOLLIN。
     *     输出非空只订阅OUT，排空finish后恢复IN/RDHUP，避免半关闭慢写的RDHUP空转。
     *     满连接容量暂停监听IN、腾位恢复。局部ADD/MOD/响应异常关闭该连接，诊断保首因；
     *     accept资源错误/监听MOD/等待错误关闭整个Loop，重复查询保failed首因、零I/O。
     *
     * @note 接入分配失败全Loop终止ENOMEM；客户端分配异常局部终止。无跨线程stop唤醒、
     *     优雅排空、worker、业务字段、全局RSS/解析CPU硬预算或服务性能承诺。
     */
    [[nodiscard]] LoopStep run_once(int timeout_ms) noexcept;
    /** @brief 立即DEL有效注册并关闭所有资源，返回首个清理错误；不重试close，无优雅排空。 */
    [[nodiscard]] std::error_code close() noexcept;

private:
    struct Slot final {
        std::unique_ptr<Connection> connection; ///< 独占，固定slot地址，释放后为空。
        std::uint64_t token = 0; ///< 当前注册身份；连接已释放则0，不复用旧token。
        std::uint32_t mask = 0;  ///< 最后一次成功注册的完整掩码，未变不MOD。
        bool runnable = false; ///< 缓存帧/读预算后需要下一轮主动服务，不表示内核就绪。
        bool serviced = false; ///< 当前轮已经服务；避免缓存与内核事件双重计预算。
    };
    std::vector<Slot> slots_; ///< 创建时定长，接入不增长，所属线程维护。
    Listener listener_;       ///< 独占监听；满容量时暂停普通IN，终止时关闭。
    UniqueFd epoll_;          ///< 独占LT注册集合，最后关闭。
    const ProtocolLoopConfig config_; ///< 固定预算和容量，无动态配置。
    std::size_t active_ = 0;          ///< slots中非空连接数，不超过配置容量。
    bool listener_paused_ = false;    ///< 成功MOD为0后true，恢复IN后false。
    std::uint64_t next_token_ = 1; ///< 0表示64位身份耗尽，拒绝继续接入，不回绕复用。
    std::error_code terminal_error_; ///< failed首因，清理错误不覆盖。
    ProtocolLoop(std::vector<Slot> &&slots,
        Listener &&listener,
        UniqueFd &&epoll,
        const ProtocolLoopConfig &config) noexcept;
    void fail(LoopStep &result, std::error_code error) noexcept;
    bool refresh_listener(LoopStep &result) noexcept;
    bool drop(std::size_t index, LoopStep &result, std::error_code error = {}) noexcept;
    bool accept_ready(LoopStep &result) noexcept;
    bool service(std::size_t index, LoopStep &result) noexcept;
};
} // namespace siftwing::net
