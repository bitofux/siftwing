/*
 * PROJECT : SIFTWING
 * FILE    : search_task.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 只拥有业务请求与本地Body预算的可移动任务载荷
 */
#pragma once
#include "siftwing/net/frame_codec.hpp"

namespace siftwing::concurrency {
/**
 * @brief 拥有已通过帧准入的SEARCH/SUGGEST请求，独立于Buffer、Connection和生产者输入
 * @note 只定义跨线程载荷，不继承WorkerTask、不执行查询、不保存回调/连接指针/fd/索引借用。
 *     request_id只关联请求，不是路由身份；RoutedSearchTask拥有连接身份/generation，所属Sub核验身份与期限。
 *     move-only且移动不抛；移空对象仅能观测、重新移动赋值或析构，request返回nullptr。
 *     不同拥有者经队列同步交接；const/unique_ptr本身不允许同一对象并发修改。
 */
class SearchTask final {
public:
    /**
     * @brief 先完整准入，再仅允许SEARCH/SUGGEST，成功才移动源
     * @param[in,out] source
     *     独占Frame左值；拒绝/异常保持全部字段。成功后request_id=0、body为空，其他字段保持。
     * @param[in] max_body_bytes
     *     [2,frame_max_body_bytes]，任务保存此本地预算，Completion用相同预算构造响应。
     *
     * @return
     *     拥有SearchTask或ProtocolError；validate_frame的首因优先，合法其他请求拒message_type。
     * @throws
     *     非法预算invalid_argument；准入分配等异常传播，提交点之前源保持。
     * @note Body严格UTF-8/JSON对象准入不代表query/key/k等业务字段合法，不重写原始字节。
     */
    [[nodiscard]] static std::variant<SearchTask, net::ProtocolError> create(
        net::Frame &source, std::size_t max_body_bytes = net::frame_max_body_bytes);
    /** @brief 禁止隐式复制请求Body；复制需求由调用方明确构造独立Frame。 */
    SearchTask(const SearchTask &) = delete;
    SearchTask &operator=(const SearchTask &) = delete;
    /** @brief 移动拥有请求/预算，源request_id与预算归零、Body清空。 */
    SearchTask(SearchTask &&other) noexcept;
    /** @brief 释放原拥有Body再移动；自移动保持，源移空。 */
    SearchTask &operator=(SearchTask &&other) noexcept;
    ~SearchTask() noexcept = default;
    /** @brief true表示非移空载荷，不表示连接仍存活或业务字段已通过验证。 */
    [[nodiscard]] bool valid() const noexcept;
    /**
     * @brief 左值只读借用请求；移空nullptr
     * @return
     *     借用由this拥有，移动/赋值/析构后失效，不延长生命周期；跨线程不得保存此借用。
     */
    [[nodiscard]] const net::Frame *request() const & noexcept;
    const net::Frame *request() const && = delete;
    /** @brief 按值返回本地Body预算；移空返回0，不是任务字节/RSS或全局在途预算。 */
    [[nodiscard]] std::size_t body_limit() const noexcept;

private:
    SearchTask(net::Frame &&source, std::size_t limit) noexcept;
    net::Frame request_; ///< 独占请求Body和关联号；构造完成后只能整体移动。
    std::size_t body_limit_; ///< 创建时冻结的准入/响应预算，移空为0。
};
} // namespace siftwing::concurrency
