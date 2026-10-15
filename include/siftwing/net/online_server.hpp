/*
 * PROJECT : SIFTWING
 * FILE    : online_server.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 固定Main/Sub/worker与只读服务的生产闭环控制者
 */
#pragma once
#include "siftwing/net/protocol_sub_reactor.hpp"
#include "siftwing/query/search_service.hpp"
#include "siftwing/concurrency/worker_pool.hpp"

namespace siftwing::net {
/** @brief 完整启动前固定的配置；默认loopback，条目预算不表示全局内存硬限。 */
struct OnlineServerConfig {
    ListenConfig listen;       ///< 沿Listener数值地址合同，默认127.0.0.1/端口0。
    std::size_t sub_count = 2; ///< [1,64]，各Sub身份为0..count-1，不动态扩缩容。
    std::size_t workers = 4;   ///< [1,64]固定worker，服务在所有线程退出后才销毁。
    std::size_t task_capacity = 128; ///< [1,1024]等待条目，不含执行中任务；full不等空位。
    ProtocolSubConfig
        sub; ///< 复用Sub固定预算；reactor_id须0、submitter/logger/version须空；控制者覆盖server_info_body。
    logging::Logger *logger = nullptr; ///< 覆盖server销毁、最后worker/Sub返回的稳定可选借用。
    MainReactorConfig main; ///< 复用Main有限接入预算。
};
/** @brief 控制线程取得的报告；运行时是快照，停止后worker/Sub全部静止。 */
struct OnlineServerReport {
    std::size_t accepted = 0, handed_off = 0, rejected = 0; ///< Main累计接入/移交/拒绝。
    concurrency::WorkerPoolReport workers; ///< 只证明execute返回，不证明网络送达。
    std::vector<ProtocolSubStatus> subs;   ///< 各Sub自己的同步快照。
    std::error_code first_error; ///< Main/Sub/结果发布首因；局部业务错误不在此计入。
    bool stopped = false;        ///< stop或shutdown已完成静止屏障和资源清理。
};
/**
 * @brief 创建线程拥有Main、固定Sub/worker与SearchService；worker只处理拥有数据
 * @note 所有公共API和析构由创建线程串行调用，无跨线程控制借用；不可copy/move/重启。
 *     Main在调用线程接入，Sub独占Connection，worker共享固定只读查询资源和服务内同步的可选LRU。
 *     worker等待队列full只拒绝本次任务：Sub返回同ID OVERLOADED；可等mutex，不等容量。
 *     错误输出仍服从Sub硬限/局部关闭，排空后可继续缓存请求；容量释放后允许新提交。
 *     可选期限沿sub配置，默认关闭；到期关连接但不取消worker。stop立即丢弃，shutdown限时排空网络。
 */
class OnlineServer final {
public:
    /**
     * @brief 全部Sub/结果目标就绪后开放监听；失败按同一发布者静止顺序回滚
     * @param[in] service
     *     按值接管非空完整服务；成功/失败均不返还已移动所有者。
     * @param[in] config
     *     按值取得固定配置，预检非法配置不启动线程或I/O。
     *
     * @return
     *     完整拥有server或generic errno；配置EINVAL、分配ENOMEM、其他异常EIO/原system_error。
     * @throws
     *     不可恢复同步/join设施损坏沿底层合同，回滚不能伪造静止保证。
     */
    [[nodiscard]] static std::variant<std::unique_ptr<OnlineServer>, std::error_code> create(
        std::unique_ptr<query::SearchService> service, OnlineServerConfig config = {});
    /** @brief 创建线程在无外部调用时stop兜底；同步设施不可恢复则terminate。 */
    ~OnlineServer() noexcept;
    OnlineServer(const OnlineServer &) = delete;
    OnlineServer &operator=(const OnlineServer &) = delete;
    OnlineServer(OnlineServer &&) = delete;
    OnlineServer &operator=(OnlineServer &&) = delete;
    /** @brief 创建线程取得实际监听端口；停止后0，无fd借用。 */
    [[nodiscard]] std::uint16_t port() const noexcept;
    /**
     * @brief 有限接入一轮，同时检查Sub/结果发布首因，失败立即执行stop
     * @param[in] timeout_ms
     *     [0,100]；非法EINVAL且状态保持，已经停止优先返回首因。
     *
     * @return
     *     空为仍可运行；业务INVALID_REQUEST/RESOURCE_LIMIT只是响应，不终止服务。
     * @throws
     *     控制线程同步/join异常可传播；析构仍须撤销全部调用入口。
     */
    [[nodiscard]] std::error_code run_once(int timeout_ms);
    /**
     * @brief Main关闭返回→Sub静止提交→worker完整join→撤目标/release→Sub join，幂等
     * @return
     *     首因或首清理诊断；结果full/通知异常不回滚已accepted条目、不重投。
     * @note 等待已接收任务完成，无硬时间上限；返回才允许销毁服务/submitter/目标。
     * @throws
     *     控制同步/join异常沿底层合同，不把closed/empty当作静止证明。
     */
    [[nodiscard]] std::error_code stop();
    /**
     * @brief 关闭接入/任务准入，在同一绝对期限内排空已在途响应，再完成全部线程/fd屏障
     * @param[in] timeout_ms
     *     [0,86400000]；0立即截止，非法EINVAL且运行态保持；已经停止优先返回原首因。
     *
     * @return
     *     首因或首清理诊断；正常网络截止不作为错误，数量见各Sub的drain_timeouts。
     * @note
     *     仅创建线程串行调用。截止不取消已accepted任务，不证明对端收到完整响应。
     *     超期后worker仍须完整执行/发布/join，通知fd保持到最后发布调用返回；无总退出硬上限。
     *     故障回退stop，析构仍用stop；重复调用不重启、不延长网络期限。
     * @throws
     *     控制同步/join异常沿底层合同；不detach线程或缩短服务/目标生命期。
     */
    [[nodiscard]] std::error_code shutdown(std::int64_t timeout_ms = 1000);
    /** @brief 创建线程汇总同步快照；mutex/复制分配异常传播，不延长任何借用。 */
    [[nodiscard]] OnlineServerReport report() const;

private:
    struct Impl;
    explicit OnlineServer(std::unique_ptr<Impl> state) noexcept;
    std::unique_ptr<Impl> state_; ///< 独占控制状态；析构前依次完成发布者屏障。
};
} // namespace siftwing::net
