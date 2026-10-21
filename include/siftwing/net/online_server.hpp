/*
 * PROJECT : SIFTWING
 * FILE    : online_server.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 应用调用线程驱动的唯一生产Reactor与现有业务线程池
 */
#pragma once
#include "siftwing/net/protocol_sub_reactor.hpp"
#include "siftwing/query/search_service.hpp"
#include "siftwing/concurrency/worker_pool.hpp"

namespace siftwing::net {
/** @brief 唯一生产循环的接入与信号策略；不包含Sub线程或handoff配置。 */
struct OnlineReactorConfig {
    std::size_t accept_attempt_budget = 16; ///< [1,1024]每轮accept4尝试上限。
    bool tcp_no_delay = false;              ///< 新接入socket显式选择TCP_NODELAY。
    bool termination_signals = false; ///< 创建worker前由调用者屏蔽INT/TERM，组件不改mask。
};
/** @brief 完整启动前固定的配置；默认loopback，条目预算不表示全局内存硬限。 */
struct OnlineServerConfig {
    ListenConfig listen;     ///< 沿Listener数值地址合同，默认127.0.0.1/端口0。
    std::size_t workers = 4; ///< [1,64]固定worker，服务在所有线程退出后才销毁。
    std::size_t task_capacity = 128; ///< [1,1024]等待条目，不含执行中任务；full不等空位。
    ProtocolConnectionConfig connection = [] {
        ProtocolConnectionConfig value;
        value.max_connections = 256;
        value.completion_capacity = 256;
        return value;
    }(); ///< 总容量各[1,65536]；身份须0，submitter/logger/version/service_info须空，由控制者绑定。
    logging::Logger *logger = nullptr; ///< 覆盖server销毁及最后worker/Core调用的稳定可选借用。
    OnlineReactorConfig reactor; ///< 唯一循环有限接入预算，不创建额外I/O线程。
};
/** @brief 所属调用线程取得的报告；运行时是值快照，停止后worker/Loader及网络全部静止。 */
struct OnlineServerReport {
    std::size_t accepted = 0,
                rejected = 0; ///< accept成功/注册前接入设施拒绝；连接容量拒绝另见reactor。
    concurrency::WorkerPoolReport workers; ///< 只证明execute返回，不证明网络送达。
    ProtocolSubStatus
        reactor; ///< 复用连接核心计数值类型，不表示拥有Sub线程；状态最终stopped而非joined。
    std::error_code first_error; ///< Reactor/结果发布首因；局部业务错误不在此计入。
    bool stopped = false;        ///< stop或shutdown已完成静止屏障和资源清理。
};
/**
 * @brief 创建线程拥有唯一Reactor、固定worker与SearchService；worker只处理拥有数据
 * @note 所有公共API和析构由创建线程串行调用，无跨线程控制借用；不可copy/move/重启。
 *     调用线程独占Listener/Connection/全部epoll和控制fd，run_once主动推进，无Main→Sub交接。
 *     worker共享只读查询资源和服务内同步LRU；任务full返回同ID OVERLOADED，不等容量。
 *     输出硬限/局部关闭、缓存主动续处理保持；可选期限沿connection配置，默认关闭。
 *     stop立即丢弃；shutdown在同一调用线程继续推进在途响应，不创建辅助I/O线程。
 */
class OnlineServer final {
public:
    /**
     * @brief 固定连接核心、结果目标与worker就绪后开放监听；失败按发布者静止顺序回滚
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
     * @brief 同线程推进有限接入、连接I/O、期限及结果一轮，故障立即执行stop
     * @param[in] timeout_ms
     *     [0,100]；非法EINVAL且状态保持，已经停止优先返回首因。
     *
     * @return
     *     空为无故障；启用signalfd后须观察shutdown_signal()并由调用者选择shutdown期限。
     *     业务INVALID_REQUEST/RESOURCE_LIMIT只是响应，不终止服务。
     * @throws
     *     控制线程同步/join异常可传播；析构仍须撤销全部调用入口。
     */
    [[nodiscard]] std::error_code run_once(int timeout_ms);
    /**
     * @brief 创建线程串行观察Reactor已消费的首个正常终止信号，关闭后仍保留
     * @return
     *     0表示尚未观察，其他为SIGINT/SIGTERM；不自动shutdown、不覆盖运行故障。
     * @note 启用config.reactor.termination_signals前须在线程创建前屏蔽两信号，所有相关线程保持。
     *     本组件不设置或恢复mask；默认禁用，库调用者自行编排进程信号策略。
     */
    [[nodiscard]] int shutdown_signal() const noexcept;
    /**
     * @brief 同线程关闭监听/任务准入及连接，关闭结果准入，worker完整join后撤目标和控制fd，幂等
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
     *     首因或首清理诊断；正常网络截止不作为错误，数量见reactor.drain_timeouts。
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
    /**
     * @brief 创建线程顺序采样组件指标，返回无借用值；SERVER_INFO语义保持，report已为单Reactor
     * @return
     *     各组件锁下的值，跨组件非原子；busy不含任务析构，延迟不含排队/网络，计数饱和。
     * @throws
     *     组件观察加锁异常传播；日志导出入口捕获，不能因此改变业务结果。
     */
    [[nodiscard]] metrics::Snapshot metrics() const;

    /** @brief 创建线程提交拥有加载请求；停止后stopped，首次按需创建线程，分配/创建异常传播。 */
    [[nodiscard]] snapshot::SnapshotLoadSubmit try_load_snapshot(
        snapshot::SnapshotLoadRequest request);
    /** @brief 创建线程取完成值，停止后仍可取；不自动发布，加载异常沿原完成值交付。 */
    [[nodiscard]] std::optional<snapshot::SnapshotLoadCompletion> take_loaded_snapshot();
    /** @brief 创建线程取当前拥有句柄，可保留为回滚目标；停止后仍可观察。 */
    [[nodiscard]] snapshot::SnapshotHandle current_snapshot() const noexcept;
    /**
     * @brief 创建线程显式发布完整候选，沿服务/Manager合同，不分配或访问文件
     * @param[in] candidate
     *     按值共享只读Reader，固定分词/停用资源相容性由调用方保证。
     *
     * @return
     *     Manager判定，或停止后ECANCELED；拒绝不改变current。run_once不隐式取结果或发布。
     */
    [[nodiscard]] std::variant<snapshot::SnapshotPublishResult, std::error_code> publish_snapshot(
        snapshot::SnapshotHandle candidate) noexcept;

private:
    struct Impl;
    explicit OnlineServer(std::unique_ptr<Impl> state) noexcept;
    std::unique_ptr<Impl> state_; ///< 独占控制状态；析构前依次完成发布者屏障。
};
} // namespace siftwing::net
