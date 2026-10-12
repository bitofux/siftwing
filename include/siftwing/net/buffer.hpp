/*
 * PROJECT : SIFTWING
 * FILE    : buffer.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 拥有连续字节存储，提供有界追加、借用和前缀消费合同
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace siftwing::net {

/**
 * @brief 单线程所属的连续字节缓冲区，不解释协议或调用系统 I/O
 *
 * 不变量为 0 <= read_ <= write_ <= capacity_ <= max_bytes_；可读区为
 * [read_, write_)。上限限制拥有的字节数组及未消费数据，不是进程 RSS 硬上限；
 * 增长期间旧、新数组会同时存活，压缩自追加也可能使用临时副本。
 *
 * @note 对象独占数组，禁止复制和移动；同一对象的读写需要调用者串行化。
 *     clear/消费不缩容，销毁才释放数组。不承诺擦除消费过的数据。
 */
class Buffer final {
public:
    /**
     * @brief 创建明确上限的 Buffer，默认延迟分配
     *
     * @param[in] max_bytes
     *     不可改变的存储上限；零表示只接受空追加。
     * @param[in] initial_capacity
     *     初始数组大小，必须不超过 max_bytes。
     *
     *
     * @throws
     *     初始大小越界抛 std::invalid_argument；分配异常直接传播。
     */
    explicit Buffer(std::size_t max_bytes, std::size_t initial_capacity = 0);
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&) = delete;
    Buffer &operator=(Buffer &&) = delete;

    /** @brief 返回未消费的字节数。 */
    [[nodiscard]] std::size_t readable_bytes() const noexcept {
        return write_ - read_;
    }
    /** @brief 返回无需压缩或增长即可追加的尾部空间。 */
    [[nodiscard]] std::size_t writable_bytes() const noexcept {
        return capacity_ - write_;
    }
    /** @brief 返回当前拥有的数组大小。 */
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    /** @brief 返回构造时指定的存储上限。 */
    [[nodiscard]] std::size_t max_bytes() const noexcept {
        return max_bytes_;
    }

    /**
     * @brief 借用可读区，空区返回空 view
     *
     * @return
     *     由本对象拥有的只读字节视图，包含任意二进制字节，无 NUL 终止保证。
     *     销毁或任何成功的非空追加、非零消费、clear 后须重新获取；失败或空操作保持有效。
     *
     * @note codec 应先确认数据完整再消费；视图不能交给跨线程任务长期保存。
     */
    [[nodiscard]] std::string_view readable_view() const noexcept;

    /**
     * @brief 原子追加完整字节串，优先复用尾部，其次压缩，最后有界增长
     * @param[in] bytes
     *     调用期间有效的借用输入，可为本对象可读区的子视图；内部处理重叠。
     *
     * @return
     *     true 表示完整追加（空输入也成功）；超过剩余预算返回 false，所有状态和视图保持。
     *
     * @throws
     *     数组或临时副本分配异常直接传播；异常发生时所有状态和原可读数据保持。
     *
     * @note 上限根据未消费数据检查，不根据累计输入量；成功不意味着组成完整帧。
     */
    [[nodiscard]] bool append(std::string_view bytes);

    /**
     * @brief 消费可读区前缀，全部消费后索引归零
     * @param[in] bytes
     *     请求消费的字节数；零是成功的空操作。
     *
     * @return
     *     不超过可读长度时 true；越界 false 且不改变任何状态。
     */
    [[nodiscard]] bool consume(std::size_t bytes) noexcept;
    /** @brief 丢弃全部可读数据并归零索引，保留容量，不抛异常。 */
    void clear() noexcept;

private:
    std::unique_ptr<char[]> storage_; ///< 独占数组，仅 [read_, write_) 可被调用者读取。
    const std::size_t max_bytes_;     ///< 终身不变的数组与可读字节上限。
    std::size_t capacity_ = 0;        ///< 当前数组大小；零时 storage_ 为空。
    std::size_t read_ = 0;            ///< 未消费前缀起点，仅所属线程更新。
    std::size_t write_ = 0;           ///< 已写数据末端，不小于 read_。
};

} // namespace siftwing::net
