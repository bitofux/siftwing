/*
 * PROJECT : SIFTWING
 * FILE    : completion.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 脱离请求生命周期的拥有型响应载荷
 */
#pragma once
#include "siftwing/concurrency/search_task.hpp"

namespace siftwing::concurrency {
/**
 * @brief 独占已准入响应Frame，不借用task、Body输入、Connection或fd
 * @note move-only/noexcept移动；尚无路由身份、队列、eventfd、发送、业务执行或自动异常映射。
 *     不可仅凭request_id回投连接。独立对象可交接；同一对象仍需外部同步/唯一拥有者。
 */
class Completion final {
public:
    /**
     * @brief 按task的预算复制响应Body并准入，映射SEARCH/SUGGEST响应且原值回传关联号
     * @param[in] task
     *     调用期间稳定的只读借用；移空返回request_id错误，不解引用空请求。
     * @param[in] body
     *     稳定同步借用，成功返回独立副本；须严格UTF-8/JSON对象并容纳于task预算。
     *
     * @return
     *     拥有Completion或codec首个ProtocolError，不改变task/body，不发布部分结果。
     * @throws
     *     复用codec的分配等异常传播，输入保持；不截断超限输出，不执行查询。
     */
    [[nodiscard]] static std::variant<Completion, net::ProtocolError> success(
        const SearchTask &task, std::string_view body);
    /**
     * @brief 复用固定安全诊断构造ERROR_RESPONSE，原值回传关联号
     * @param[in] task
     *     稳定借用，移空返回request_id错误；错误响应使用task保存的同一Body预算。
     * @param[in] status
     *     codec六种ErrorStatus之一，未知值返回status错误。
     *
     * @return
     *     拥有Completion或ProtocolError；预算太小返回body_length，不递归生成错误。
     * @throws
     *     分配等异常传播且task保持；不泄露任意异常文本，不证明响应已经交付。
     */
    [[nodiscard]] static std::variant<Completion, net::ProtocolError> error(
        const SearchTask &task, net::ErrorStatus status);
    Completion(const Completion &) = delete;
    Completion &operator=(const Completion &) = delete;
    /** @brief 移动响应，源request_id归零、Body清空，不抛异常。 */
    Completion(Completion &&other) noexcept;
    /** @brief 释放原响应再转交；自移动保持，源移空。 */
    Completion &operator=(Completion &&other) noexcept;
    ~Completion() noexcept = default;
    /** @brief 表示非移空完整响应，不证明目标连接仍存活或结果已经发送。 */
    [[nodiscard]] bool valid() const noexcept;
    /** @brief 左值只读借用拥有Frame；移空nullptr；移动/赋值/析构失效，不能跨线程保存。 */
    [[nodiscard]] const net::Frame *frame() const & noexcept;
    const net::Frame *frame() const && = delete;
    /**
     * @brief 从非const右值取得拥有Frame，源移空；不编码或发送
     * @return
     *     独立拥有Frame；移空对象返回request_id=0、Body为空的无效Frame，调用者须检查。
     */
    [[nodiscard]] net::Frame take_frame() && noexcept;

private:
    explicit Completion(net::Frame &&frame) noexcept;
    net::Frame frame_; ///< 独占响应字节及关联号；只有整体移动/取出会改动。
};
} // namespace siftwing::concurrency
