/*
 * PROJECT : SIFTWING
 * FILE    : task_queue.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 独占任务的有界提交、阻塞消费与关闭排空
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace siftwing::concurrency {
/** @brief 只有accepted转移源；closed优先于invalid_task，后者优先于full。 */
enum class TaskSubmitResult {
    accepted,
    closed,
    invalid_task,
    full
};
/** @brief 同一锁内的按值观测，不预留容量，不证明外部调用者已经静止。 */
struct TaskQueueStatus {
    std::size_t capacity; ///< 构造时固定的正条目上限，不是任务字节/RSS或在途任务上限。
    std::size_t size; ///< 尚未取出的条目，取出后不再占队列容量。
    bool closed;      ///< 单调关闭提交入口；已有任务仍可消费。
};
/**
 * @brief 多生产者/多消费者的固定容量FIFO，只交接独占任务，不执行或创建线程
 * @tparam T 完整任务类型，析构不抛；任务可以不可复制、不可移动，队列只移动unique_ptr。
 * @note 正常提交/取出不分配、不调用任务函数或析构任务；try_submit可等mutex但不等空位。
 *     每个源由其调用者独占，提交前写入通过队列锁发布给消费者；不提供任务内部并发同步。
 *     FIFO指锁内提交/取出顺序，多个消费者的执行/完成顺序不受保证。
 *     所有调用要求this存活。close后撤销并等待生产者、唤醒并join消费者，再销毁；
 *     closed/empty不证明调用者已返回。析构不自动close/join，残留任务在析构线程销毁。
 */
template <class T>
class TaskQueue final {
    static_assert(std::is_nothrow_destructible_v<T>, "task destruction must not throw");

public:
    /**
     * @brief 验证正容量后一次性预分配空槽；不创建任务/线程或I/O资源
     * @throws 0抛invalid_argument，超vector范围抛length_error，分配失败抛bad_alloc。
     */
    explicit TaskQueue(std::size_t capacity) {
        if (capacity == 0)
            throw std::invalid_argument("task queue capacity must be positive");
        slots_.resize(capacity);
    }
    /** @brief 仅在无活跃调用者时析构；残留unique_ptr兜底，不替代close/drain/join协议。 */
    ~TaskQueue() noexcept = default;
    TaskQueue(const TaskQueue &) = delete;
    TaskQueue &operator=(const TaskQueue &) = delete;
    TaskQueue(TaskQueue &&) = delete;
    TaskQueue &operator=(TaskQueue &&) = delete;

    /**
     * @brief 尝试提交一个拥有者，不等待容量；锁内move是提交点
     * @param[in,out] source accepted后为空，所有拒绝及提交前加锁失败保持原指针和任务。
     * @return 按closed、invalid_task、full、accepted顺序决定；成功后锁外notify_one。
     * @throws mutex加锁失败可抛system_error；不接受临时拥有者，不承诺硬实时/公平性。
     */
    [[nodiscard]] TaskSubmitResult try_submit(std::unique_ptr<T> &source) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (closed_)
                return TaskSubmitResult::closed;
            if (!source)
                return TaskSubmitResult::invalid_task;
            if (size_ == slots_.size())
                return TaskSubmitResult::full;
            // 活跃环区之外必为空；用距环尾的距离计算tail，避免head_ + size_溢出。
            const auto gap = slots_.size() - head_;
            const auto tail = size_ >= gap ? size_ - gap : head_ + size_;
            slots_[tail] = std::move(source);
            ++size_;
        }
        available_.notify_one();
        return TaskSubmitResult::accepted;
    }
    /** @brief 非阻塞取出最早任务；开放或关闭的空队列均返回空指针，加锁失败可抛。 */
    [[nodiscard]] std::unique_ptr<T> try_pop() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return pop_locked();
    }
    /**
     * @brief 等待有任务或关闭；关闭后仍按FIFO排空，只有closed且empty返回空指针
     * @return 独占任务，锁释放后由消费者执行/销毁；任务执行策略属于调用方。
     * @throws 初次加锁/条件等待系统错误可抛；无法重新持锁时标准库可能terminate。
     * @note 谓词在同一mutex内读取，接受通知先到或虚假唤醒；遗漏close可无限等待。
     */
    [[nodiscard]] std::unique_ptr<T> wait_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [this] {
            return size_ != 0 || closed_;
        });
        return pop_locked();
    }
    /**
     * @brief 同一锁内幂等关闭提交入口、不清条目，锁外notify_all唤醒所有消费者
     * @throws 加锁失败可抛system_error且状态保持；不等待发布者、任务执行或线程退出。
     */
    void close() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        available_.notify_all();
    }
    /** @brief 加锁复制最近状态；加锁失败可抛system_error，不暴露任务借用。 */
    [[nodiscard]] TaskQueueStatus status() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return {slots_.size(), size_, closed_};
    }

private:
    /** @brief 调用者持mutex；移动后原槽为空，锁内不会析构用户任务。 */
    std::unique_ptr<T> pop_locked() noexcept {
        if (size_ == 0)
            return {};
        auto task = std::move(slots_[head_]);
        ++head_;
        if (head_ == slots_.size())
            head_ = 0;
        --size_;
        return task;
    }
    std::vector<std::unique_ptr<T>> slots_; ///< 固定空槽，活跃环区各拥有一个非空任务。
    mutable std::mutex mutex_; ///< 保护槽位/head/size/closed，提供任务发布可见性。
    std::condition_variable available_; ///< 只提示检查谓词，通知次数不等于任务数量。
    std::size_t head_ = 0;              ///< 下一取出位置，总小于固定容量。
    std::size_t size_ = 0; ///< 队列内拥有条目数；消费者已取出的任务不计入。
    bool closed_ = false;  ///< 不可重开；关闭与提交以同一个锁排序。
};
} // namespace siftwing::concurrency
