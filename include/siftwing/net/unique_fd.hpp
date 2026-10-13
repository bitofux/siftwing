/*
 * PROJECT : SIFTWING
 * FILE    : unique_fd.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : Linux文件描述符的独占RAII拥有者，关闭一次并支持显式转交
 */
#pragma once

#include <system_error>

namespace siftwing::net {
/**
 * @brief 独占一个Linux fd；负值为空，fd 0也是有效资源
 *
 * @note 接管前必须保证fd由调用方独占；不能为同一fd创建两个拥有者，也不能在外部关闭get()。
 *     不检查fd是否打开，不复制、不dup、不shutdown。移动/release后源为空；同一对象及其fd
 *     的使用、移动、关闭须串行化，跨线程转交需要调用方同步。
 */
class UniqueFd final {
public:
    /** @brief 创建空拥有者，不分配资源。 */
    UniqueFd() noexcept = default;
    /**
     * @brief 接管fd，不调用系统调用
     * @param[in] fd
     *     独占的非负fd；任意负值规范化为空的-1。
     */
    explicit UniqueFd(int fd) noexcept;
    /** @brief 关闭一次，忽略关闭错误，保持调用前errno；不会抛异常。 */
    ~UniqueFd() noexcept;
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    /** @brief 转交fd，源变为空，不关闭资源。 */
    UniqueFd(UniqueFd &&other) noexcept;
    /** @brief 关闭旧fd一次后接管源，源为空；自移动保持，关闭错误忽略、errno保持。 */
    UniqueFd &operator=(UniqueFd &&other) noexcept;

    /**
     * @brief 借用fd，空时返回-1
     * @return
     *     数字不携带所有权；close/release/移动/析构后不得继续作为本拥有者的借用使用。
     *     关闭后数字可能立即被内核复用，数字相同不代表同一资源。
     */
    [[nodiscard]] int get() const & noexcept {
        return fd_;
    }
    /** @brief 禁止从临时拥有者取得很快失效的借用fd。 */
    int get() const && = delete;
    /** @brief 查询是否拥有非负fd，不检查内核当前状态。 */
    explicit operator bool() const noexcept {
        return fd_ >= 0;
    }
    /**
     * @brief 放弃管理并转交原fd，源立即为空，不关闭
     * @return
     *     原fd或-1；非负返回值由调用方负责最终关闭或交给另一个唯一拥有者。
     */
    [[nodiscard]] int release() noexcept;
    /**
     * @brief 先清空拥有者，再调用一次Linux close；空对象是成功的无操作
     * @return
     *     成功为空error_code，失败保存generic_category下的原errno；调用前errno保持。
     * @note Linux即使EINTR也释放描述符槽，不能重试close或再次接管该数字；真实关闭错误
     *     仅作诊断。析构不能报告错误，需诊断时在析构前显式调用本函数。
     */
    [[nodiscard]] std::error_code close() noexcept;

private:
    int fd_ = -1; ///< 独占fd或空哨兵；只有本对象的串行操作能改变。
};
} // namespace siftwing::net
