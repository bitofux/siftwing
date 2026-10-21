/*
 * PROJECT : SIFTWING
 * FILE    : completion_queue.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 单个Reactor的有界拥有结果邮箱，非阻塞容量与关闭排空
 */
#pragma once
#include "siftwing/concurrency/routed_payload.hpp"
#include <memory>
#include <mutex>
#include <vector>

namespace siftwing::concurrency {
/** @brief 发布结论；所有拒绝均保持源，顺序closed→invalid_completion→wrong_reactor→full。 */
enum class CompletionPublishResult {
    accepted,
    closed,
    invalid_completion,
    wrong_reactor,
    full
};
/** @brief 单一mutex中的按值观测；不代表发布者已经返回或连接仍存活。 */
struct CompletionQueueStatus {
    std::uint32_t reactor_id; ///< 构造冻结的目的Reactor编号，0合法。
    std::size_t capacity; ///< 固定[1,65536]条目容量，不是Body字节/RSS或执行中任务上限。
    std::size_t size; ///< 尚未被取出的拥有结果数，陈旧结果也占容量。
    bool closed;      ///< 单向关闭发布入口；已有结果仍可取出。
};
/**
 * @brief 固定一个Reactor目的地的有界FIFO；只拥有结果，不访问Connection/身份表或执行通知
 * @note 发布/取出/关闭/观测由同一mutex同步；锁可能等待，不等待空位，不承诺硬实时或公平性。
 *     预期多个worker发布、所属I/O线程消费；队列不验证调用线程身份，只核路由编号。
 *     worker无法知道generation是否过期。取出后所属线程须核当前身份、Connection和请求状态，
 *     陈旧结果在锁外销毁。队列不自动扫描/驱逐、不设置时钟/deadline，也不发送响应。
 *     full保留新结果，调用者负责明确处理；不得把拒绝当已交付。try_publish_and_notify另行组合eventfd通知。
 *     析构前撤销所有入口并等待调用返回；close/empty不证明调用者静止，析构不自动排空。
 */
class CompletionQueue final {
public:
    /**
     * @brief 冻结目的编号并预分配空槽，不创建结果/线程/通知资源
     * @param[in] reactor_id
     *     与接收Sub身份表相同的编号；不同Sub命名空间及生命周期仍由控制者管理。
     * @param[in] capacity
     *     固定正条目上限[1,65536]。
     *
     * @throws
     *     容量非法invalid_argument，预分配bad_alloc/length_error；无部分发布对象。
     */
    CompletionQueue(std::uint32_t reactor_id, std::size_t capacity);
    /** @brief 无活跃调用者时释放空槽及残留结果；残留在析构线程销毁，无网络操作。 */
    ~CompletionQueue() noexcept = default;
    CompletionQueue(const CompletionQueue &) = delete;
    CompletionQueue &operator=(const CompletionQueue &) = delete;
    CompletionQueue(CompletionQueue &&) = delete;
    CompletionQueue &operator=(CompletionQueue &&) = delete;
    /**
     * @brief 核关闭、有效载荷、目的编号与容量，成功才移动拥有者到环尾
     * @param[in,out] source
     *     独占稳定unique_ptr左值；accepted移空，任何拒绝或提交前加锁异常保持原对象。
     *
     * @return
     *     按closed→invalid_completion→wrong_reactor→full→accepted决定；不证明generation存活。
     * @throws
     *     mutex加锁system_error，状态与源保持。提交之后无分配、不抛；消费者可能在返回前取出。
     */
    [[nodiscard]] CompletionPublishResult try_publish(std::unique_ptr<RoutedCompletion> &source);
    /**
     * @brief 取出最早拥有结果，不等待条目；closed时仍排空，空时返回空指针
     * @return
     *     独占结果，锁外检查路由/应用或丢弃；不提供任何内部借用。
     * @throws
     *     初次mutex加锁system_error且保持队列；成功取出不分配、不抛。
     */
    [[nodiscard]] std::unique_ptr<RoutedCompletion> try_pop();
    /** @brief 幂等关闭发布入口，保留已接收条目；不等待生产者/消费/通知，加锁失败可抛。 */
    void close();
    /** @brief 同锁按值复制状态；加锁system_error可抛，观测不保留连接或this生命周期。 */
    [[nodiscard]] CompletionQueueStatus status() const;

private:
    const std::uint32_t reactor_id_; ///< 固定目的地；不取自fd、request_id或线程id。
    std::vector<std::unique_ptr<RoutedCompletion>> slots_; ///< 固定空槽，活跃环区独占结果。
    mutable std::mutex mutex_; ///< 保护head/size/closed及槽位，建立结果发布可见性。
    std::size_t head_ = 0;     ///< 下一取出位置，始终小于capacity。
    std::size_t size_ = 0;     ///< 当前环区条目数，已取出结果不计入。
    bool closed_ = false;      ///< 不可重开；与发布以同锁排序。
};
} // namespace siftwing::concurrency
