/*
 * PROJECT : SIFTWING
 * FILE    : socket_ops.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 非阻塞TCP socket创建与单次收发的状态及错误合同
 */
#pragma once

#include "siftwing/net/unique_fd.hpp"
#include <cstddef>
#include <string_view>
#include <system_error>
#include <variant>

namespace siftwing::net {
/** @brief TCP地址族选择，不携带地址、端口或连接状态。 */
enum class IpFamily {
    v4,
    v6
};
/** @brief 成功分支独占新socket，失败分支拥有原errno对应的error_code。 */
using SocketResult = std::variant<UniqueFd, std::error_code>;
/** @brief progress是本次局部进展；其他分支不表示应用层帧或业务完成。 */
enum class IoState {
    progress,
    would_block,
    interrupted,
    eof,
    error
};
/**
 * @brief 单次系统收发的值结果，无fd或输入存储的拥有权
 * @note progress时error为空、bytes为实际字节数（空请求为0）；eof仅非空recv返回0且error为空。
 *     其余分支bytes=0、error保存errno；调用者按bytes推进自己的缓冲区并安排下一次调用。
 */
struct IoResult final {
    IoState state; ///< 本次进展/暂不可用/中断/读取EOF/真实错误；调用者必须分别处理。
    std::size_t bytes; ///< 本次传输量，不是累计量；不代表对端应用已处理。
    std::error_code error; ///< generic_category下的errno，非错误进展与EOF为空。
};
/**
 * @brief 单次socket调用创建TCP fd，原子设置SOCK_NONBLOCK和SOCK_CLOEXEC
 * @param[in] family
 *     v4或v6；其他枚举值返回EINVAL且不调用内核。
 * @return
 *     独占UniqueFd或原系统error_code（包括EINTR/EMFILE等），失败无泄漏、无部分对象。
 * @note 不隐藏重试，无fcntl退化或全局配置；不bind/listen/connect，IPv6仍取内核默认策略。
 */
[[nodiscard]] SocketResult open_tcp_socket(IpFamily family = IpFamily::v4) noexcept;
/**
 * @brief 最多一次recv，强制本次MSG_DONTWAIT，不自动填满缓冲区或重试EINTR
 * @param[in] fd
 *     稳定借用已连接的流socket，调用期间不得关闭或复用；负值返回EBADF。
 * @param[out] destination
 *     调用期间有效可写存储；仅进展分支写入前bytes字节，非零capacity时不能为空。
 * @param[in] capacity
 *     最多读取量，不超过ssize_t上限；0不调用内核，返回progress/0，不能判为EOF。
 * @return
 *     短读也是progress；EAGAIN/EWOULDBLOCK为would_block，EINTR为interrupted，0为eof，其他为error。
 * @note 参数错误返回EINVAL/EBADF、bytes0，无分配/异常，不承诺保持errno。MSG_DONTWAIT不改变fd标志；
 *     不更新Buffer、关闭连接或解释codec。EINTR交由拥有调用预算的上层安排，避免隐藏无限循环。
 */
[[nodiscard]] IoResult receive_some(int fd, char *destination, std::size_t capacity) noexcept;
/**
 * @brief 最多一次send，使用MSG_DONTWAIT|MSG_NOSIGNAL，不隐藏重试或补齐短写
 * @param[in] fd
 *     稳定借用已连接流socket，调用期间不得关闭或复用；负值返回EBADF。
 * @param[in] bytes
 *     调用期间稳定借用的原始字节；大小不超过ssize_t上限，空输入返回progress/0、不调用内核。
 * @return
 *     返回本次实际进展或would_block/interrupted/error；发送不返回eof，EPIPE是error。
 * @note 不改进程/线程SIGPIPE策略，MSG_NOSIGNAL只抑制本次调用的SIGPIPE；成功仅表示内核接受字节。
 *     不持有输入、不分配、不抛异常，不承诺errno保持；调用方推进余量并决定重试、等待或关闭。
 */
[[nodiscard]] IoResult send_some(int fd, std::string_view bytes) noexcept;
} // namespace siftwing::net
