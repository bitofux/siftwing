/*
 * PROJECT : SIFTWING
 * FILE    : connection.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 单线程所属连接的有界收发、响应关联和在途门禁
 */
#pragma once

#include "siftwing/net/frame_codec.hpp"
#include "siftwing/net/unique_fd.hpp"
#include <system_error>

namespace siftwing::net {
/** @brief 本轮读取停止原因；budget_exhausted不表示内核或用户缓冲区已经排空。 */
enum class ReadStop {
    request, ///< 完整头合法，request有值；Body拒绝详body_error，不交给业务执行。
    request_pending,  ///< 上一交付尚未finish_request，零解码/recv。
    would_block,      ///< recv的EAGAIN/EWOULDBLOCK，保留半帧，error为原errno。
    budget_exhausted, ///< 字节或尝试预算到达，保留余量，error为空。
    invalid_budget,   ///< 零字节或零尝试预算，EINVAL，零解码/recv且状态保持。
    peer_eof,         ///< 空输入时recv返回0；读取终止并关闭fd。
    truncated,        ///< EOF时残留不完整帧；读取终止，不发布部分请求。
    header_error,     ///< decode_next拒绝头；不猜ID或重同步，终止并关闭。
    io_error,         ///< 收发系统失败；终止，error保存首个I/O诊断。
    local_failure, ///< 本地分配/库异常曾传播；连接已终止，后续查询此状态。
    closed         ///< 显式关闭；后续调用无I/O。
};
/**
 * @brief 拥有本轮结果；只有request分支携带独立Frame，计数仅指本轮
 *
 * @note request分支body_error为空才可交业务；有值时仅允许用受信头生成错误响应。
 *     header_error仅在同名分支有值。除would_block/io_error/invalid_budget外error为空。
 *     已终止对象重复查询时保留原因/诊断，attempts/bytes为0，不重复关闭。
 */
struct ReadResult final {
    ReadStop stop = ReadStop::budget_exhausted; ///< 本轮停止原因，不代表响应已发送。
    std::size_t attempts = 0; ///< 实际recv尝试次数，包含EINTR、EAGAIN、EOF及失败。
    std::size_t bytes = 0;    ///< 本轮从内核取得的字节数；已缓存帧交付可为0。
    std::optional<Frame> request;            ///< 独占Body，连接关闭或销毁后仍有效。
    std::optional<DecodeError> header_error; ///< 头拒绝首因，无受信请求ID。
    std::optional<ProtocolError> body_error; ///< 完整合法头的Body拒绝，不验证业务字段。
    std::error_code error; ///< 原系统errno或零预算EINVAL；不是线上status。
};

/** @brief 响应入队结果；所有拒绝都不改变连接、输出或调用者的Frame。 */
enum class QueueStop {
    queued,           ///< 完整线字节已复制进输出Buffer，尚未send。
    closed,           ///< 连接已终止。
    no_request,       ///< 没有本连接交付的在途请求。
    response_pending, ///< 本请求已接纳一个响应，即使已排空也不能重复入队。
    request_mismatch, ///< ID/成功类型不匹配；Body拒绝的请求只能用错误响应。
    protocol_error    ///< codec拒绝响应字段或Body，详protocol_error。
};
/** @brief 值结果不拥有Frame；protocol_error只在同名分支有值。 */
struct QueueResult final {
    QueueStop stop;
    std::optional<ProtocolError> protocol_error = {};
};
/** @brief 排空只证明字节交给本机内核，不证明对端接收或业务处理完成。 */
enum class WriteStop {
    drained,          ///< 输出为空；应关闭EPOLLOUT，可由上层finish_request。
    would_block,      ///< EAGAIN/EWOULDBLOCK，保留精确未发送后缀和写意向。
    budget_exhausted, ///< 字节/尝试预算到达，仍有后缀，应继续保留写意向。
    invalid_budget,   ///< 非空输出遇零预算；EINVAL，无send且状态保持。
    io_error,         ///< 发送失败或非空send无进展；连接关闭，输出丢弃。
    closed            ///< 其他原因终止，无send；error可携带原终止I/O诊断。
};
/** @brief 本轮send计数及停止原因；已终止重复查询计数为0，原诊断保持。 */
struct WriteResult final {
    WriteStop stop = WriteStop::drained;
    std::size_t attempts = 0; ///< 每次send均计数，包含EINTR/EAGAIN/错误。
    std::size_t bytes = 0; ///< 本轮实际交给内核的字节，不计已缓存/上轮数据。
    std::error_code error; ///< 原errno、invalid_budget的EINVAL或无进展的EIO。
};

/**
 * @brief 独占已连接流socket和输入/输出Buffer，每次最多交付一个请求
 *
 * @note 同一对象只由所属I/O线程串行操作；不复制、不移动、不自动注册epoll，不含worker、
 *     回调或业务执行。每请求至多一个完整响应；输出上限同输入为24+max_body_bytes。
 *     get可供所属线程使用；使用本输出链路时不得另行send打乱字节顺序或外部close。
 *     不同Connection可独立使用；const查询不提供同步，fd数字不替代连接身份。
 */
class Connection final {
public:
    /**
     * @brief 配置和初始存储成功后才转交fd
     *
     * @param[in,out] socket
     *     非空UniqueFd，独占已连接的流socket（通常来自Listener）；成功后源为空。
     *
     * @param[in] max_body_bytes
     *     [2,frame_max_body_bytes]；输入上限固定为24+此值，允许最大一帧。
     *
     * @param[in] initial_capacity
     *     不大于输入上限；默认延迟分配。
     *
     * @throws
     *     空fd/非法配置抛invalid_argument，分配异常传播；失败时socket完全保持。
     *
     * @note 不探测socket类型或连接状态；调用者保证前置条件。receive_some强制MSG_DONTWAIT。
     */
    explicit Connection(UniqueFd &&socket,
        std::size_t max_body_bytes = frame_max_body_bytes,
        std::size_t initial_capacity = 0);
    ~Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;

    /** @brief 借用fd；close、终止或析构后失效，已终止返回-1，fd 0合法。 */
    [[nodiscard]] int get() const & noexcept {
        return socket_.get();
    }
    int get() const && = delete;
    /** @brief 返回尚未消费的输入字节数，不暴露Buffer view。 */
    [[nodiscard]] std::size_t buffered_bytes() const noexcept {
        return input_.readable_bytes();
    }
    /** @brief 查询上一请求（包括Body拒绝）是否尚未完成。 */
    [[nodiscard]] bool request_pending() const noexcept {
        return pending_;
    }
    /** @brief 尚未交给内核的输出字节数；不暴露内部view。 */
    [[nodiscard]] std::size_t output_bytes() const noexcept {
        return output_.readable_bytes();
    }
    /**
     * @brief 输出非空且fd有效时需要EPOLLOUT；空输出/终止时为false
     *
     * @note 这是意向，不是已经成功注册；所属Loop负责用epoll MOD更新掩码，失败时关闭。
     *     上层可入队后立即flush；EAGAIN/预算后保留写事件，排空后关闭，避免LT空转。
     */
    [[nodiscard]] bool needs_write() const noexcept {
        return socket_.get() >= 0 && output_.readable_bytes() != 0;
    }

    /**
     * @brief 为本连接的在途请求原子接纳一个拥有型响应
     *
     * @param[in] response
     *     同步稳定借用；ID须匹配，成功类型须对应请求，错误响应须通过codec。
     *
     * @return
     *     按closed→no_request→response_pending→request_mismatch→codec错误顺序检查。
     *     Body拒绝的请求只接纳ERROR_RESPONSE；成功编码为客户端方向后复制完整线字节。
     *
     * @throws
     *     编码/JSON/Buffer分配异常原样传播，输入、输出、fd和请求门禁保持，可重试。
     *
     * @note 不send、不自动finish；输入与输出各最多一帧，临时编码会增加峰值内存。
     */
    [[nodiscard]] QueueResult queue_response(const Frame &response);
    /**
     * @brief 按字节/尝试双预算发送输出前缀，保留短写后缀
     *
     * @param[in] byte_budget
     *     正数，本轮最多交给内核的字节数。
     *
     * @param[in] attempt_budget
     *     正数，每次send计数，EINTR有界继续；EAGAIN立即返回。
     *
     * @return
     *     先检查终止/空输出，再拒绝零预算。最后一次send若排空，drained优先于预算。
     *     错误关闭一次并丢弃输入/输出/门禁，首因不被close覆盖；非空send返回0用EIO。
     *
     * @note send_some强制MSG_DONTWAIT|MSG_NOSIGNAL；无分配，无异常，不自动解除请求门禁。
     *     drained后上层finish_request，再主动调度已缓存下一帧；不只等待新EPOLLIN。
     */
    [[nodiscard]] WriteResult flush_output(
        std::size_t byte_budget = 65536, std::size_t attempt_budget = 16) noexcept;

    /**
     * @brief 先拆缓存首帧，再按双预算recv，最多交付一帧
     *
     * @param[in] byte_budget
     *     正数，本轮最多从内核取得的字节数，不计已缓存字节。
     *
     * @param[in] attempt_budget
     *     正数，每次recv均计预算；EINTR有界继续，EAGAIN立即停止。
     *
     * @return
     *     详ReadResult；先处理已终止、在途门禁，再拒绝零预算。最后一次读取后仍检查完整帧，
     *     请求/头拒绝优先于预算耗尽。缓存的下一帧在finish后可零recv交付，调用方须主动调度，
     *     不能只等待内核新就绪。半帧不消费、不交付，短读继续直到上述边界。
     *
     * @throws
     *     Buffer/Body/JSON分配或库异常传播；捕获时先终止并关闭、丢弃输入，再原样抛出。
     *     已从内核读取的字节不能回滚，因此异常后不可重试该流；已交付Frame保持独立。
     *
     * @note EOF/非法头/系统失败自动关闭一次；关闭诊断不覆盖原读取错误。EOF在完成在途请求后
     *     才继续观察，不把SHUT_WR当作立即丢弃完整请求。字节/尝试预算不限制解析CPU或全局RSS。
     */
    [[nodiscard]] ReadResult read_request(
        std::size_t byte_budget = 65536, std::size_t attempt_budget = 16);
    /**
     * @brief 上层确认响应完成后解除门禁，不自动读取
     *
     * @return
     *     有在途请求、fd有效且输出已空时true，否则false且保持；Body错误也须完成错误响应或close。
     *
     * @warning 本层不能证明业务/网络完成；调用方须在逻辑响应实际写完后调用，不能刚提交任务就解除。
     */
    [[nodiscard]] bool finish_request() noexcept;
    /** @brief 显式丢弃输入输出/解除门禁/关闭一次；空fd成功，关闭错误仍不重试。 */
    [[nodiscard]] std::error_code close() noexcept;

private:
    // 声明顺序保证Buffer构造/配置失败时尚未移动调用方fd。
    Buffer input_;    ///< 独占有界数组，终止清可读区，不缩容。
    Buffer output_;   ///< 独占完整响应的未发送后缀，consume推进写偏移。
    UniqueFd socket_; ///< 已连接fd，终止/close清空，析构只关闭一次。
    const std::size_t max_body_bytes_; ///< 同时用于拆帧和Body准入的固定上限。
    bool pending_ = false; ///< 已交付但尚未完成的逻辑请求，所属线程维护。
    bool response_queued_ = false; ///< 本请求已接纳响应，排空后仍防重复发送。
    bool body_rejected_ = false;   ///< Body错误只允许关联错误响应。
    std::uint64_t pending_id_ = 0; ///< 在途受信请求ID；不用外部Frame的可变副本当状态。
    MessageType pending_type_ = MessageType::ping; ///< 在途受信请求类型。
    WriteStop write_terminal_ = WriteStop::closed; ///< 发送失败的重复查询保持io_error。
    ReadStop terminal_ = ReadStop::closed; ///< fd为空时的终止原因；fd有效时不读取。
    std::optional<DecodeError> terminal_header_; ///< 终止头诊断，无Body借用。
    std::error_code terminal_error_; ///< 终止系统诊断，关闭错误不能覆盖。
    void terminate(ReadStop reason,
        std::optional<DecodeError> header = {},
        std::error_code error = {}) noexcept;
};
} // namespace siftwing::net
