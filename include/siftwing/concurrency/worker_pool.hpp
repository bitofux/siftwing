/*
 * PROJECT : SIFTWING
 * FILE    : worker_pool.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 固定业务线程、独占任务执行与关闭排空
 */
#pragma once
#include "siftwing/concurrency/task_queue.hpp"
#include <cstdint>
#include <exception>
#include <thread>

namespace siftwing::concurrency {
/**
 * @brief 独占的可执行任务基类；具体任务负责拥有其输入与依赖的有效生命周期
 * @note execute可以抛异常，由pool隔离；析构不得抛，accepted后在执行worker销毁。
 *     不允许携带借用Connection或用fd数字代表连接；SearchTask/Completion定义请求与响应载荷，业务服务执行查询。
 */
class WorkerTask {
public:
    virtual ~WorkerTask() noexcept = default;
    /** @brief 由一个worker调用一次；成功/异常均不自动重试或生成业务响应。 */
    virtual void execute() = 0;
protected:
    WorkerTask() = default;
    WorkerTask(const WorkerTask &) = delete;
    WorkerTask &operator=(const WorkerTask &) = delete;
};
/** @brief 报告锁内按值快照，统计到execute返回或抛出，不保证对应任务析构已完成。 */
struct WorkerPoolReport {
    std::uint64_t succeeded = 0; ///< 成功执行数，达到uint64_t最大值后饱和。
    std::uint64_t failed = 0; ///< execute抛出数，饱和计数；失败不停止其他任务。
    std::exception_ptr first_exception; ///< 首个记录的失败拥有引用；可重抛，销毁不必发生在worker。
};
/**
 * @brief 一次启动的固定线程池；复用有界TaskQueue，关闭后完成所有已接收任务再退出
 * @note 不可复制/移动/重启。提交和request_stop/观测可并发；join由非本池worker调用，
 *     多个join以内部锁串行。所有调用者须保证this存活；join不等待外部生产者调用返回。
 *     销毁前撤销所有外部入口并等调用返回；不得在本池任务中销毁pool。
 *     任务可以request_stop或再提交，但不能join自身pool；不承诺完成顺序或公平性。
 *     pool同步设施失效为不可恢复错误：worker边界/析构中terminate，不伪装任务失败。
 */
class WorkerPool final {
public:
    /**
     * @brief 启动1—64个worker；全部进入启动门禁后才返回开放对象
     * @param[in] workers 固定线程数，不自动依据硬件调整。
     * @param[in] capacity 固定正待执行条目容量；不包括执行中任务/任务字节/RSS。
     *
     * @throws 非法worker数/零容量invalid_argument，容量范围length_error，分配bad_alloc，
     *     线程创建或构造者同步操作可抛system_error。部分启动先释放退出门禁并join已创建线程，
     *     再传播原异常；回滚同步/join无法完成则terminate。
     */
    WorkerPool(std::size_t workers, std::size_t capacity);
    /** @brief 无外部调用者时自动close/drain/join；任务永久阻塞可使析构无限等待。 */
    ~WorkerPool() noexcept;
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;
    WorkerPool(WorkerPool &&) = delete;
    WorkerPool &operator=(WorkerPool &&) = delete;
    /**
     * @brief 复用TaskQueue提交合同，可等mutex，不等队列空位
     * @param[in,out] source accepted后为空，closed/invalid_task/full与初次锁失败保持源。
     *
     * @return 与TaskQueue同优先顺序；只移动unique_ptr<WorkerTask>左值。
     * @throws 提交前mutex加锁system_error；成功任务可能在返回前已经执行/销毁。
     */
    [[nodiscard]] TaskSubmitResult try_submit(std::unique_ptr<WorkerTask> &source);
    /** @brief 幂等关闭入口并唤醒worker；保留已接收任务，不等待执行/线程/外部调用者；锁失败可抛。 */
    void request_stop();
    /**
     * @brief 先关闭再等待所有worker，重复/并发join串行且幂等
     * @throws 本池worker调用抛logic_error且不关闭入口；锁/线程join系统错误可抛。
     * @note 无取消/截止时间；任务阻塞或循环可使join无限等待。返回后任务均执行并销毁，
     *     但外部submit/stop/观测调用仍须调用者自行撤销并等待。
     */
    void join();
    /** @brief 返回队列自己的同锁快照；不包括执行中任务，不证明全部调用已静止；锁失败可抛。 */
    [[nodiscard]] TaskQueueStatus queue_status() const;
    /** @brief 复制统计与首异常的同锁快照；join后为最终统计；锁失败可抛。 */
    [[nodiscard]] WorkerPoolReport report() const;
private:
    void worker_main() noexcept;
    void rollback_start() noexcept;
    TaskQueue<WorkerTask> queue_; ///< 成功源独占交接给worker，旧队列合同不变。
    std::vector<std::thread> threads_; ///< 构造者启动，join锁保护控制者回收；worker不访问vector。
    std::mutex start_mutex_; ///< 保护ready/start/abort，构造者与启动worker共享。
    std::condition_variable start_cv_; ///< 所有worker就绪后统一放行；失败统一放行退出。
    std::size_t ready_ = 0; ///< 已进入门禁的worker数，仅构造阶段写入。
    bool start_ = false; ///< 单向启动发布，构造者设置。
    bool abort_ = false; ///< 启动失败退出，不允许消费任务。
    std::mutex join_mutex_; ///< 串行化控制者join，绝不在持此锁时执行任务。
    mutable std::mutex report_mutex_; ///< 保护execute统计与首异常，不包围用户execute/析构。
    WorkerPoolReport report_; ///< 报告拥有首异常，后续异常不累积保存。
    static thread_local WorkerPool *current_; ///< 仅所属线程读写的身份，用于阻止self-join。
};
} // namespace siftwing::concurrency
