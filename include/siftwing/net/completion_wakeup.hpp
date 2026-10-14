/*
 * PROJECT : SIFTWING
 * FILE    : completion_wakeup.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 拥有结果的两阶段发布通知与所属Sub的请求提交边界
 */
#pragma once
#include "siftwing/concurrency/completion_queue.hpp"
#include "siftwing/concurrency/task_queue.hpp"
#include "siftwing/net/eventfd_wakeup.hpp"

namespace siftwing::net {
/** @brief 同一Sub的稳定发布借用；不授予Connection或身份表访问，release后失效。 */
struct CompletionTarget {
    concurrency::CompletionQueue *queue; ///< 真实结果存储；控制者保证整个发布调用期间存活。
    int wakeup_fd; ///< 所属Sub的非阻塞普通eventfd；须等所有发布及通知返回后才关闭。
};
/** @brief 入队与通知独立；通知失败不能撤销accepted，也不能重投已消费的源。 */
struct CompletionWakeupResult {
    concurrency::CompletionPublishResult publish; ///< 只有accepted消费源，拒绝保持源。
    std::error_code notification_error; ///< 拒绝时为空；accepted后仍须处理此诊断。
};
/**
 * @brief 先向真实队列提交独占结果，再在队列锁外通知，不等待容量
 * @param[in,out] queue
 *     存活的所属队列，发布语义沿用CompletionQueue。
 * @param[in,out] source
 *     独占稳定左值；拒绝及提交前锁异常保持，accepted移空。
 * @param[in] stable_wakeup_fd
 *     同notify_eventfd的稳定借用，覆盖整个调用及最后控制通知。
 *
 * @return
 *     两阶段结果。EAGAIN表示已有通知；其他通知错误保留原诊断，不收回已提交结果。
 * @throws
 *     queue初次锁异常发生在提交前；通知不抛。full保留新结果，由调用者处理。
 * @note
 *     消费者可能在返回前已取出结果。控制者停止提交、等待worker及完整通知调用返回、
 *     撤销所有借用后才release发布者；closed/empty不能代替此屏障。
 */
[[nodiscard]] CompletionWakeupResult try_publish_and_notify(concurrency::CompletionQueue &queue,
    std::unique_ptr<concurrency::RoutedCompletion> &source,
    int stable_wakeup_fd);

/**
 * @brief 所属I/O线程调用的可选提交接口；只交接拥有请求，不执行查询或等待空位
 * @note
 *     控制者在start前配置，并保证到Sub join返回才销毁。实现可组合WorkerPool，
 *     accepted须移空source，拒绝保持，异常只能发生在提交前；每个接纳任务至多发布一个结果。
 *     worker只持任务/结果/CompletionTarget，不持Connection、socket或身份表借用。
 *     不向Sub同步回调应用结果，不从try_submit停止/join同一Sub。生产查询适配由接入模块定义。
 *     当前协议允许重复request_id；同连接同ID再次使用后无法识别违反“至多一个结果”合同的旧副本。
 */
class RoutedRequestSubmitter {
public:
    virtual ~RoutedRequestSubmitter() noexcept = default;
    /**
     * @brief 以TaskSubmitResult表达接纳，所有拒绝均保持源
     * @param[in,out] source
     *     有效独占请求，成功交接后为空，失败由Sub释放或生成受信错误。
     *
     * @return
     *     accepted/full/closed/invalid_task；不等待容量，可能等待实现的mutex。
     * @throws
     *     提交前分配/锁异常传播，Sub关闭该连接，不泄露异常文本到线协议。
     */
    [[nodiscard]] virtual concurrency::TaskSubmitResult try_submit(
        std::unique_ptr<concurrency::RoutedSearchTask> &source) = 0;
};
} // namespace siftwing::net
