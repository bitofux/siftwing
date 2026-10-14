/*
 * PROJECT : SIFTWING
 * FILE    : main_reactor.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 仅接入与轮询移交的Main线程调度器及跨线程停止入口
 */
#pragma once

#include "siftwing/net/eventfd_wakeup.hpp"
#include "siftwing/net/listener.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace siftwing::net {
/**
 * @brief 固定目标的借用；调用方在Main创建前保证全部就绪，不从fd数字推断就绪
 * @note 队列和非阻塞普通eventfd必须存活到Main停止发布、所有调用返回后；不许提前关闭/复用。
 *     队列只作该Main的接入端；目标自行消费/清理。本类型不拥有Sub，不判断Sub运行首因。
 */
struct MainReactorTarget {
    FdHandoffQueue *queue; ///< 非空稳定借用，各目标不同队列；创建时开放。
    int wakeup_fd;         ///< 目标稳定eventfd借用，各目标数字不同；不得由Main关闭。
};
/** @brief 启动后固定的尝试预算与新连接配置，无动态扩缩容。 */
struct MainReactorConfig {
    std::size_t accept_attempt_budget = 16; ///< [1,1024]，含EINTR/可重试网络错误，不是CPU硬预算。
    bool tcp_no_delay = false; ///< 为true时对每个新socket单次设置TCP_NODELAY；失败关闭并停止接入。
};
/** @brief 调度结果；终止后保持首因，重复查询零I/O。 */
enum class MainReactorStop { progress, idle, interrupted, invalid_timeout, failed, closed };
/** @brief 一轮统计，accepted=handed_off+rejected；通知失败仍计handed_off。 */
struct MainReactorStep {
    MainReactorStop stop = MainReactorStop::progress; ///< idle不证明未来无工作。
    std::size_t events = 0;          ///< 单次epoll_wait返回数，最多2。
    std::size_t accept_attempts = 0; ///< 实际accept4次数，包括失败和重试。
    std::size_t accepted = 0;        ///< 内核已交付socket数量。
    std::size_t handed_off = 0;      ///< 队列已消费源，目标或其消费者拥有。
    std::size_t rejected = 0;        ///< 未提交源已由Main关闭一次，不改投其他目标。
    bool accept_budget_exhausted = false; ///< 本轮预算耗尽，不证明监听队列为空，LT下后续继续。
    std::error_code error;          ///< failed首因、interrupted EINTR或invalid_timeout EINVAL。
    std::error_code cleanup_error;  ///< 首个清理诊断，不覆盖error；Linux close不重试。
};
/**
 * @brief 独占Listener/LT epoll/停止eventfd；只接入和移交，不创建线程、不读写客户端业务
 * @note create/run_once/port/close/析构由所属I/O线程串行调用；request_stop可并发，但对象须存活，
 *     析构前撤销stop入口并等调用者返回。对象不可copy/move，不拥有目标线程或目标资源。
 *     全部目标创建时就绪且开放；运行时发现目标关闭，停止整个Main，不能静默缩容。
 */
class MainReactor final {
public:
    /** @brief 最大等待100ms，停止通知失败也有检查停止的后备；无调度延迟硬上限。 */
    static constexpr int stop_poll_interval_ms = 100;
    /**
     * @brief 完整预检固定目标后创建监听/epoll/停止端并注册，成功才发布完整Main
     *
     * @param[in] targets
     *     [1,64]个已就绪目标；无空queue/负fd/重复queue/重复fd，创建时queue须开放。
     *     复制借用表不拥有资源；调用方同时保证目标未运行失败，创建不验证eventfd类型。
     * @param[in] listen
     *     数值地址仅创建时借用，合同同Listener::create。
     * @param[in] config
     *     固定合法预算与TCP_NODELAY选择。
     *
     * @return
     *     完整unique_ptr或generic errno；非法配置EINVAL，已关闭目标ECANCELED，预检不创建I/O。
     *     系统创建/注册失败关闭本次已建资源，但不关闭目标queue/eventfd或改变已有条目。
     *
     * @throws
     *     目标表/对象分配和预检mutex异常传播；已建资源RAII回收。不发布部分对象。
     */
    [[nodiscard]] static std::variant<std::unique_ptr<MainReactor>, std::error_code> create(
        const std::vector<MainReactorTarget> &targets,
        const ListenConfig &listen = {}, const MainReactorConfig &config = {});
    /** @brief 所属线程调用close；同步设施异常导致terminate，不能在并发stop调用中析构。 */
    ~MainReactor() noexcept;
    MainReactor(const MainReactor &) = delete;
    MainReactor &operator=(const MainReactor &) = delete;
    MainReactor(MainReactor &&) = delete;
    MainReactor &operator=(MainReactor &&) = delete;
    /** @brief 所属线程观察实际监听端口；终止后0，不授予fd借用。 */
    [[nodiscard]] std::uint16_t port() const noexcept { return listener_.port(); }
    /**
     * @brief 单调发布停止，再有限通知Main自己的eventfd；不直接关闭任何资源
     *
     * @return
     *     通知诊断；通知失败也保持停止标志，run_once最多100ms等待后再检查。
     *     已撤销自身通知端后为空；幂等，不等待线程退出或目标队列排空。
     *
     * @throws
     *     通知端保护mutex的同步异常可抛；停止标志已经生效。调用方仍须保证对象存活。
     */
    [[nodiscard]] std::error_code request_stop();
    /**
     * @brief 有限wait、接入和轮询移交；不调用业务recv/send，不创建Connection
     *
     * @param[in] timeout_ms
     *     [0,100]，禁止无限等待；非法值返回EINVAL且状态保持，已终止优先。
     *
     * @return
     *     EINTR不隐藏重试；accept沿Listener有限预算重试。每个新fd选择当前游标目标，
     *     每次移交尝试后前进一次；满队列关闭源、不重选、不失败；关闭目标、配置/接入/通知
     *     或移交异常停止接入，保留已提交事实。目标开放性每轮检查，发现关闭不静默缩容。
     *     系统资源错误立即停止，重复run不忙重试。失败映射system_error.code/bad_alloc ENOMEM/其他EIO。
     *
     * @throws
     *     自身通知端撤销mutex的同步异常可抛；目标queue异常在调度边界吸收，源仍由Main关闭。
     */
    [[nodiscard]] MainReactorStep run_once(int timeout_ms);
    /**
     * @brief 所属线程终止接入、尝试关闭全部目标发布入口，DEL/close自身资源，幂等
     *
     * @return
     *     首个清理诊断；目标queue关闭异常记录后继续其余目标，该入口可能仍开放，须上层协调。
     *     Main不pop或close已提交条目，重复close不重新尝试失败的目标关闭。
     *     目标队列关闭不证明其他发布者静止；上层须等Main调用返回、最后目标控制通知返回，
     *     才允许目标consumer关闭其eventfd和清理。无join或全服务停止编排。
     *
     * @throws
     *     自身通知端撤销mutex的同步异常可抛，保护与request_stop串行，避免写入复用fd。
     */
    [[nodiscard]] std::error_code close();
private:
    MainReactor(std::vector<MainReactorTarget> &&targets, Listener &&listener,
        UniqueFd &&epoll, UniqueFd &&wakeup, MainReactorConfig config) noexcept;
    void fail(MainReactorStep &step, std::error_code error);
    std::vector<MainReactorTarget> targets_; ///< 固定借用表，顺序即轮询顺序，Main不拥有目标。
    Listener listener_;      ///< 所属线程拥有，只用于accept，不读业务。
    UniqueFd epoll_;         ///< 所属线程拥有，仅监听listener与自己的停止端。
    UniqueFd wakeup_;        ///< mutex保护借用/撤销，先在锁内转交局部拥有者，再锁外close。
    std::mutex wakeup_mutex_; ///< 串行有限notify与拥有者撤销，不在锁内close/queue操作/join。
    std::atomic<bool> stop_{false}; ///< 跨线程单调请求，不表示清理完成。
    MainReactorConfig config_; ///< 创建后不变。
    std::size_t cursor_ = 0;   ///< 下一目标，小于targets_.size()；只所属线程访问。
    bool closed_ = false;     ///< 所属线程的幂等清理标志，不作跨线程报告。
    std::error_code first_error_;   ///< 所属线程首因，清理不能覆盖。
    std::error_code cleanup_error_; ///< 所属线程首个清理诊断。
};
} // namespace siftwing::net
