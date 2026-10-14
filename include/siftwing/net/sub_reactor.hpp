/*
 * PROJECT : SIFTWING
 * FILE    : sub_reactor.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 独立Sub线程的就绪、有限等待停止与所属线程资源清理
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>

namespace siftwing::net {
/** @brief 单次生命周期；stopped表示线程内清理完成，joined表示外部join也已完成。 */
enum class SubReactorState {
    idle,
    starting,
    running,
    stopped,
    joined
};
/** @brief 按值复制的同步观测；快照不保证下一时刻仍running，也不授予fd借用。 */
struct SubReactorStatus {
    SubReactorState state = SubReactorState::idle; ///< 同一锁保护的生命周期状态。
    bool stop_requested = false; ///< 单调停止标志的观测值，不等同线程退出。
    std::error_code first_error; ///< 启动或运行首因；正常停止为空，不被关闭错误覆盖。
    std::error_code cleanup_error; ///< 独立保存epoll关闭错误；Linux close不重试。
};
/**
 * @brief 拥有一个Sub线程；线程内独占epoll，目前无连接、队列或通知端点
 *
 * @note start/join/析构由外部唯一控制者串行调用；request_stop/status可由其他线程并发调用，
 *     但调用方须保证对象存活，析构前撤销入口并等待调用者返回。不可复制或移动，线程借用this。
 *     不暴露epoll fd，不监听端口，不调用业务回调；本层不编排整个服务的启动与停止。
 */
class SubReactor final {
public:
    /** @brief 每次epoll_wait最多等待100ms；调度延迟无硬实时上限，EINTR后重新检查停止。 */
    static constexpr int stop_poll_interval_ms = 100;
    /** @brief 创建未启动对象；不创建线程或I/O资源。 */
    SubReactor() = default;
    /** @brief 请求停止并join；不得在所属线程析构，join/同步设施异常导致terminate。 */
    ~SubReactor() noexcept;
    SubReactor(const SubReactor &) = delete;
    SubReactor &operator=(const SubReactor &) = delete;
    SubReactor(SubReactor &&) = delete;
    SubReactor &operator=(SubReactor &&) = delete;

    /**
     * @brief 启动一次并等待线程内epoll就绪；启动失败时内部join后返回
     *
     * @return
     *     空表示曾完整就绪，随后仍可能运行失败；重复启动返回generic EALREADY。
     *     提前或启动中停止返回operation_canceled，不可重启。线程创建system_error保留code，
     *     bad_alloc映射not_enough_memory，其他异常映射io_error；epoll创建保留errno。
     *
     * @note 可能阻塞于线程启动/调度和失败回收；不保证整个服务就绪，控制者须汇总所有Sub。
     *     控制者互斥/条件变量/join的标准库系统错误可抛出；不承诺保持errno。
     */
    [[nodiscard]] std::error_code start();
    /** @brief 幂等、无阻塞地发布停止请求；idle时同样生效，不等待退出，不关闭任何fd。 */
    void request_stop() noexcept;
    /**
     * @brief 外部控制者等待线程退出；不隐含stop，running时须先request_stop
     *
     * @return
     *     首因优先，否则关闭错误；重复join保持结果，idle时无操作并允许之后start。
     *     self-join返回resource_deadlock_would_occur且保留线程；标准库join/同步错误可抛出。
     *
     * @note 不持状态锁join，成功返回后线程不再借用对象；不能与start/join/析构并发。
     */
    [[nodiscard]] std::error_code join();
    /** @brief 加锁复制状态与错误；可并发观察，同步设施系统错误可抛出。 */
    [[nodiscard]] SubReactorStatus status() const;

private:
    void run() noexcept;
    std::thread thread_; ///< 控制者独占线程句柄；只在外部start/join/析构使用。
    std::atomic<bool> stop_{false}; ///< 跨线程单调停止请求，线程每次等待前检查。
    mutable std::mutex mutex_;      ///< 仅保护报告，不保护线程句柄或I/O资源。
    std::condition_variable ready_cv_; ///< 启动报告的谓词等待，防止丢失通知/虚假唤醒。
    SubReactorState state_ = SubReactorState::idle; ///< 受mutex_保护。
    bool ready_ = false; ///< 受mutex_保护；本次启动报告已提交，成功不等于仍在运行。
    std::error_code ready_error_; ///< 受mutex_保护；保存独立启动结果，不被运行首因修改。
    std::error_code first_error_;   ///< 受mutex_保护；清理后发布首因。
    std::error_code cleanup_error_; ///< 受mutex_保护；清理后发布关闭诊断。
};
} // namespace siftwing::net
