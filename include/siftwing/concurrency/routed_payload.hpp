/*
 * PROJECT : SIFTWING
 * FILE    : routed_payload.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 身份与拥有请求/响应的不可复制包装，不携带连接借用
 */
#pragma once
#include "siftwing/concurrency/completion.hpp"
#include "siftwing/net/connection_identity.hpp"

namespace siftwing::concurrency {
/** @brief 本地包装拒绝；与codec ProtocolError及线上ErrorStatus分开。 */
enum class RouteError {
    invalid_identity,
    empty_payload
};
/**
 * @brief 同时拥有ConnectionId纯值与SearchTask，移动后源身份/载荷移空
 * @note 数值有效的身份不代表存活；只有所属Reactor身份表能校验，不能凭request_id回投。
 *     不继承WorkerTask，无Connection/fd/回调借用。不同拥有者经同步队列交接，同对象仍需外部同步。
 */
class RoutedSearchTask final {
public:
    /**
     * @brief 先核身份与有效载荷，成功才消费SearchTask左值；不重新解析或分配
     * @param[in] route
     *     由所属Reactor登记取得的纯值；generation为0拒绝，不能由fd数字或request_id猜测。
     * @param[in,out] source
     *     独占有效SearchTask；拒绝保持所有状态，成功后移空。
     *
     * @return
     *     拥有包装或invalid_identity/empty_payload，按此顺序；不抛异常。
     */
    [[nodiscard]] static std::variant<RoutedSearchTask, RouteError> create(
        net::ConnectionId route, SearchTask &source) noexcept;
    /** @brief 禁止复制拥有请求。 */
    RoutedSearchTask(const RoutedSearchTask &) = delete;
    /** @brief 禁止复制赋值拥有请求。 */
    RoutedSearchTask &operator=(const RoutedSearchTask &) = delete;
    /** @brief 移走身份/请求，源身份清零且载荷移空，不抛异常。 */
    RoutedSearchTask(RoutedSearchTask &&other) noexcept;
    /** @brief 替换原拥有数据；自移动保持，其他源移空。 */
    RoutedSearchTask &operator=(RoutedSearchTask &&other) noexcept;
    /** @brief 销毁拥有请求；不访问Connection或执行任务。 */
    ~RoutedSearchTask() noexcept = default;
    /** @brief 仅判断包装非移空，不证明连接当前存活。 */
    [[nodiscard]] bool valid() const noexcept;
    /** @brief 按值取得身份，移空返回全零；它不借用this。 */
    [[nodiscard]] net::ConnectionId route() const noexcept;
    /** @brief 左值只读借用this拥有的载荷，移空nullptr；移动/赋值/析构失效，不跨线程保存。 */
    [[nodiscard]] const SearchTask *payload() const & noexcept;
    const SearchTask *payload() const && = delete;

private:
    RoutedSearchTask(net::ConnectionId route, SearchTask &&source) noexcept;
    net::ConnectionId route_; ///< 创建冻结的目的身份纯值；整体移动时移空。
    SearchTask payload_;      ///< 独占请求，独立于输入及连接生命周期。
};
/**
 * @brief 从RoutedSearchTask构造的独立拥有结果，完整复制路由身份与原关联号
 * @note 不自动回投/验证存活/发送；接收者必须在所属线程核身份、Connection及当前请求状态。
 *     可移走不可复制；输入先销毁不影响结果，同对象并发修改需外部同步。
 */
class RoutedCompletion final {
public:
    /**
     * @brief 复用Completion::success构造响应，并原值复制task身份，不消费task
     * @param[in] task
     *     同步稳定借用；移空优先empty_payload，无表/Connection访问。
     * @param[in] body
     *     同步稳定借用，成功复制独立Body；预算/准入合同沿用Completion。
     *
     * @return
     *     拥有结果、RouteError或ProtocolError，读取variant前检查分支。
     * @throws
     *     codec分配异常传播，task/body保持；不会发布部分包装。
     */
    [[nodiscard]] static std::variant<RoutedCompletion, RouteError, net::ProtocolError> success(
        const RoutedSearchTask &task, std::string_view body);
    /**
     * @brief 复用六种固定安全错误响应并原值复制身份，不泄露任意异常文本
     * @param[in] task
     *     稳定同步借用；移空empty_payload，构造不证明目的地还存在。
     * @param[in] status
     *     六种ErrorStatus之一；未知值和小Body预算沿用codec拒绝。
     *
     * @return
     *     拥有结果、RouteError或ProtocolError，不消费task；不递归生成错误。
     * @throws
     *     分配异常传播且输入保持。
     */
    [[nodiscard]] static std::variant<RoutedCompletion, RouteError, net::ProtocolError> error(
        const RoutedSearchTask &task, net::ErrorStatus status);
    /** @brief 禁止复制响应Body。 */
    RoutedCompletion(const RoutedCompletion &) = delete;
    /** @brief 禁止复制赋值响应Body。 */
    RoutedCompletion &operator=(const RoutedCompletion &) = delete;
    /** @brief 转交身份/结果，源身份清零且载荷移空。 */
    RoutedCompletion(RoutedCompletion &&other) noexcept;
    /** @brief 替换原结果，自移动保持，其他源移空。 */
    RoutedCompletion &operator=(RoutedCompletion &&other) noexcept;
    /** @brief 释放响应Body，不代表响应已发送或应用。 */
    ~RoutedCompletion() noexcept = default;
    /** @brief 表示非移空结果，不证明身份仍存活或已交付。 */
    [[nodiscard]] bool valid() const noexcept;
    /** @brief 按值取得身份，移空全零；接收者用身份表校验。 */
    [[nodiscard]] net::ConnectionId route() const noexcept;
    /** @brief 左值只读借用拥有Completion，移空nullptr；移动/赋值/析构失效，不跨线程保存。 */
    [[nodiscard]] const Completion *payload() const & noexcept;
    const Completion *payload() const && = delete;

private:
    RoutedCompletion(net::ConnectionId route, Completion &&source) noexcept;
    net::ConnectionId route_; ///< 成功工厂从对应task复制，禁止单独改写目的地。
    Completion payload_;      ///< 独占响应，不借用task或Body输入。
};
} // namespace siftwing::concurrency
