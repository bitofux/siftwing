/*
 * PROJECT : SIFTWING
 * FILE    : logger.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 固定结构与上下文值的可选同步日志，不拥有业务对象
 */
#pragma once
#include "siftwing/base/result.hpp"
#include "siftwing/net/connection_identity.hpp"
#include "siftwing/metrics/server_metrics.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace siftwing::logging {
/** @brief 最低记录级别；off禁用，debug包含全部事件。 */
enum class Level {
    debug,
    info,
    warn,
    error,
    off
};
/** @brief 固定模块名，不能向日志传递任意用户文本。 */
enum class Module {
    server,
    sub,
    worker,
    cache
};
/** @brief 事件只描述本机到达的阶段，不证明对端收到或跨线程日志顺序。 */
enum class Event {
    server_starting,
    server_started,
    server_stopped,
    server_failed,
    connection_opened,
    connection_closed,
    request_received,
    task_submitted,
    task_rejected,
    cache_lookup,
    query_finished,
    completion_published,
    completion_rejected,
    completion_applied,
    completion_stale,
    output_limited,
    idle_expired,
    request_expired
};
/** @brief 稳定分类；native_error另存数字，不记录任意异常消息或路径。 */
enum class Code {
    ok,
    service_load,
    server_io,
    local_exception,
    io_error,
    timeout,
    queue_full,
    queue_closed,
    invalid_task,
    business_error,
    stale_identity,
    output_limit,
    publication_failed,
    protocol_error
};
/** @brief 精确单调用状态；not_checked表示未达到lookup，不能推断miss。 */
enum class CacheState {
    not_checked,
    disabled,
    miss,
    hit,
    unavailable
};
/** @brief 文件大小轮转参数；只在非空file时使用，无后台线程或时间轮转。 */
struct Rotation final {
    std::size_t max_bytes = 10 * 1024 * 1024; ///< [2048,1GiB]；完整行写入前判断，单行不拆分。
    std::size_t max_files = 3; ///< [1,20]份归档，另有一个当前文件；最旧归档由spdlog移除。
};
/** @brief 全为拥有标量；optional区分缺失与合法0/maxuint64，不能保存正文或资源路径。 */
struct Record final {
    Level level = Level::info;      ///< 本事件级别，与实例最低级别比较。
    Module module = Module::server; ///< 实际记录的逻辑模块，线程ID在write时取得。
    Event event = Event::server_started;     ///< 固定事件，无任意message字段。
    Code code = Code::ok;                    ///< 稳定错误分类，不等于errno。
    std::optional<std::uint32_t> reactor_id; ///< 无连接的Sub事件仍可携带所属编号。
    std::optional<net::ConnectionId> connection; ///< 完整身份值；日志不查表、不借用连接。
    std::optional<std::uint64_t> request_id, index_version; ///< 十进制字符串输出，保留完整uint64。
    std::optional<std::uint64_t> elapsed_us, result_count; ///< 单次查询实测耗时/完整成功数组长度。
    CacheState cache_state = CacheState::not_checked; ///< not_checked输出null，其余为固定名称。
    int native_error = 0; ///< errno或线上ErrorStatus的数字，含义由code区分。
};
/**
 * @brief 显式拥有spdlog同步实例与输出fd，无全局注册或后台线程
 * @note 不可copy/move；并发enabled/write/write_query/failed安全，析构前调用者撤入口并等待返回。
 *     logger必须覆盖借用它的server、worker与Sub完整生命期。同步I/O可阻塞，无硬时间保证。
 *     输出失败置failed并停用后续输出；可能留下部分末行，不递归诊断、不改变业务结果。
 */
class Logger final {
public:
    /**
     * @brief 启动线程建立独占输出目标；非空file自动使用spdlog大小轮转
     * @param[in] level
     *     固定最低级别；off仅允许空file，禁用实例不创建fd。
     * @param[in] file
     *     空为dup启动时stderr；否则相对启动cwd、逐组件拒..和链接，O_EXCL/0600新建普通文件。
     *     不创建父目录；启动拒绝已有当前文件及配置范围内的归档。父目录须由当前用户拥有且
     *     不允许组/其他用户写入；运行中本文件族仅由本实例管理，禁止外部轮转/同名写入。
     *     失败可能保留空文件/部分轮转结果，不回滚。输入只在本调用借用。
     * @param[in] rotation
     *     有界文件大小/归档份数，stderr不使用；非法值返回log.argument。
     *
     * @return
     *     完整拥有Logger或log.argument/log.file错误；预期失败不开放部分Logger。
     * @throws
     *     分配/未知库异常传播，局部资源由RAII回收；无fsync或恶意同用户并发改目录保证。
     */
    [[nodiscard]] static base::Result<std::unique_ptr<Logger>> create(
        Level level, std::string_view file = {}, Rotation rotation = {});
    /** @brief 全部调用入口撤销并等待返回后，释放同步实例与独占fd。 */
    ~Logger() noexcept;
    /** @brief 独占日志实例不可复制。 */
    Logger(const Logger &) = delete;
    /** @brief 禁止复制赋值。 */
    Logger &operator=(const Logger &) = delete;
    /** @brief 地址稳定，禁止移动使外部借用失效。 */
    Logger(Logger &&) = delete;
    /** @brief 禁止移动赋值。 */
    Logger &operator=(Logger &&) = delete;
    /** @brief 线程安全阈值快照；false不序列化/不执行I/O，不保证下一刻仍健康。 */
    [[nodiscard]] bool enabled(Level level) const noexcept;
    /** @brief 线程安全失败标志；只由false变true，不是全部日志送达或磁盘持久性的证明。 */
    [[nodiscard]] bool failed() const noexcept;
    /** @brief 同步借用Record，只输出固定schema，正常行<=2KiB；异常/短写失败吞并并置failed。 */
    void write(const Record &record) noexcept;
    /**
     * @brief 为成功查询从有界JSON Body提取results数组长度，其余字节不写入日志
     * @param[in] record
     *     上下文值；result_count由本调用替换，调用者不传异常文本。
     * @param[in] body
     *     [2,1MiB]、深度<=64的完整成功Body，仅同步借用；禁用时不解析，解析异常置failed但不传播。
     */
    void write_query(Record record, std::string_view body) noexcept;
    /**
     * @brief info级输出固定10族指标，普通事件schema保持；同步借用值，不保存引用
     * @param[in] snapshot
     *     调用期间稳定的拥有值，无动态标签；每族独立JSON行仍<=2048字节，uint64用十进制字符串。
     * @param[in] sample_id
     *     调用者提供本进程样本关联号；0/max合法，不是全局唯一或原子多行送达保证。
     * @note 禁用时不序列化；分配/序列化/I/O异常只停用日志，可能已写部分族，不回滚。
     */
    void write_metrics(const metrics::Snapshot &snapshot, std::uint64_t sample_id) noexcept;

private:
    struct Impl;
    explicit Logger(std::unique_ptr<Impl> state) noexcept;
    std::unique_ptr<Impl> state_; ///< 独占实现；析构无外部并发，释放spdlog后关闭fd。
};
} // namespace siftwing::logging
