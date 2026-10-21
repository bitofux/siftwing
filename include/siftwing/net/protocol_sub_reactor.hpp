/*
 * PROJECT : SIFTWING
 * FILE    : protocol_sub_reactor.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-14
 * BRIEF   : 专用fd队列的协议Sub线程、发布者静止屏障与所属线程清理
 */
#pragma once
#include "siftwing/net/connection.hpp"
#include "siftwing/net/main_reactor.hpp"
#include "siftwing/net/completion_wakeup.hpp"
#include "siftwing/net/service_info_source.hpp"
#include "siftwing/net/protocol_connection_config.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace siftwing::logging {
class Logger;
}
namespace siftwing::net {
/** @brief 启动后固定的容量/预算；不约束全局RSS、JSON CPU或调度延迟。 */
struct ProtocolSubConfig : ProtocolConnectionConfig {
    std::size_t handoff_capacity = 128; ///< [1,1024]，专用queue条目数。
    std::size_t handoff_budget = 16; ///< [1,1024]，每轮最多pop的条目数，不按通知计数pop。
    // 旧独立Sub对继承的max_connections/completion_capacity仍限制[1,1024]，其余沿连接配置。
};
/** @brief draining只推进已在途；quiescing停止业务等发布者；stopped已清理，joined已回收线程。 */
enum class ProtocolSubState {
    idle,
    starting,
    running,
    quiescing,
    stopped,
    joined,
    draining
};
/** @brief 同一报告锁的按值观测，不授予连接借用，也不保证下一刻仍运行。 */
struct ProtocolSubStatus {
    ProtocolSubState state = ProtocolSubState::idle; ///< 生命周期，禁止从quiescing推断可销毁。
    bool stop_requested =
        false; ///< 立即停止请求，不等于线程退出或响应完成；排空另见drain_started。
    std::size_t received = 0; ///< 正常调度从queue取得的条目，含注册前拒绝。
    std::size_t admitted = 0; ///< 曾成功构造并ADD的连接数，归属本Sub直到终止。
    std::size_t rejected = 0; ///< received中未注册的源，在Sub关闭。
    std::size_t dropped = 0;  ///< 曾注册后释放的连接数，含停止时丢弃。
    std::size_t discarded = 0; ///< 排空停止接纳或发布者静止后关闭的queue残留，不计received。
    std::size_t active = 0;    ///< 最近发布的活跃连接数，最终为0。
    std::size_t requests = 0;  ///< 完整受信请求数，含Body拒绝或未接业务的类型。
    std::size_t responses = 0; ///< 输出排空且finish成功，不证明对端收到。
    std::error_code first_error;            ///< 启动/运行首因，正常停止为空。
    std::error_code cleanup_error;          ///< 首个独立清理诊断，不覆盖运行首因。
    std::error_code first_connection_error; ///< 局部客户端错误，不自动终止Sub。
    std::size_t completion_received = 0; ///< 正常调度取出结果数，等于applied+stale+rejected。
    std::size_t completion_applied = 0; ///< 已复制结果或硬限替代ERROR，不证明发送完成。
    std::size_t completion_stale = 0; ///< 身份表拒绝的旧代次/未知slot结果，锁外销毁。
    std::size_t completion_rejected = 0; ///< 当前身份但无等待请求/不匹配/应用异常的结果。
    std::size_t completion_discarded = 0; ///< 停止且发布者静止后直接销毁的残留，不应用。
    std::size_t output_limit_rejections = 0; ///< 合法关联响应被输出硬限拒绝次数，含直接错误响应。
    std::size_t output_limit_closes =
        0; ///< 硬限拒绝后固定RESOURCE_LIMIT也无法入队/构造的局部关闭。
    std::size_t idle_timeouts = 0; ///< 空闲期限到期而关闭的连接数；不含停止清理。
    std::size_t request_timeouts = 0; ///< 请求期限到期而关闭的连接数，包含输出未排空。
    metrics::ConnectionIo io; ///< 已关闭与仍活跃连接累计I/O之和；高水位取所有曾存活连接的最大值。
    std::uint64_t input_bytes = 0, output_bytes = 0; ///< 最近发布的活跃连接用户缓冲总可读字节。
    std::uint64_t protocol_errors = 0; ///< 坏头/截帧各一次，完整坏Body逐请求一次，业务错误不计。
    std::array<std::uint64_t, 4> request_types{}; ///< PING/INFO/SEARCH/SUGGEST交付数，包括坏Body。
    std::array<std::uint64_t, 7> response_statuses{}; ///< 实际成功入队的status 0..6，非网络排空。
    bool drain_started = false; ///< 所属线程已观察排空请求，此后不读取新请求。
    std::size_t drain_responses = 0; ///< 排空阶段finish成功后关闭的连接数，不证明对端收到。
    std::size_t drain_timeouts = 0; ///< 排空截止时仍pending而关闭的连接数，非服务运行错误。
};
/** @brief 两个独立队列锁下取得的即时深度，不是同一时点，也不预留容量。 */
struct SubQueueDepths {
    std::size_t handoff = 0, completions = 0;
};
/**
 * @brief 拥有专用queue与一个协议I/O线程，不监听，不accept，不访问其他Sub连接
 * @note 构造/start/target/release_publishers/join/析构由唯一外部控制者串行调用。
 *     request_stop/request_drain/status可并发，但this须存活，析构前撤入口并等调用返回。不可copy/move。
 *     默认只响应PING/PONG与受信ERROR；可选submitter交接SEARCH/SUGGEST并接收worker结果。
 *     每连接最多一个在途请求：从完整受信帧交付到响应排空保持门禁，PING和Body拒绝也串行。
 *     等结果时不订阅IN/RDHUP；有输出时只订阅OUT，不recv或交付下一帧，其他连接独立服务。
 *     排空只证明字节交本机内核；finish后主动继续缓存，不要求peer重新发送。
 *     SHUT_WR不撤销完整请求，缓存帧处理后才观察EOF；ERR/HUP和停止仍可立即丢弃。
 *     request_id可在完成后复用；generation不是请求代次，每接纳任务须至多发布一个结果。
 *     输出达到高水位暂停读，降到低水位只解除输出原因，pending仍须等drained；绝不提前finish。
 *     响应超hard尝试同ID固定RESOURCE_LIMIT；仍不适配或构造异常只关该连接，不递归或截断。
 *     不执行真实查询，不拥有submitter/worker；request_stop立即丢弃，request_drain仅排空已pending。
 *     每线程独占一个CLOCK_MONOTONIC timerfd，初始disarmed，与eventfd共用控制token0；
 *     可选空闲/请求期限默认关闭；同钟when<=now到期，关闭并撤销身份，不发送超时ERROR。
 *     空闲只在无pending时生效，正recv刷新；请求时限包括等待和输出排空，进展不延长。
 *     到期可能丢弃部分已发送响应；不取消worker，迟到结果由身份/期限门禁拒绝。
 *     每轮至多处理max_connections个到期条目；不按内核计数推断数量，无硬实时保证。
 */
class ProtocolSubReactor final {
public:
    /** @brief 每次wait最多100ms，stop通知失败仍检查标志；不是硬实时上限。 */
    static constexpr int stop_poll_interval_ms = 100;
    /**
     * @brief 完整验证固定配置后预分配queue，不创建线程或I/O资源
     * @param[in] config
     *     字段范围同ProtocolSubConfig，构造后不可变。
     * @throws
     *     非法配置invalid_argument，预分配bad_alloc/length_error传播。
     */
    explicit ProtocolSubReactor(const ProtocolSubConfig &config = {});
    /** @brief 无外部发布者/借用调用时才可析构；stop/release/join兜底，同步异常terminate。 */
    ~ProtocolSubReactor() noexcept;
    ProtocolSubReactor(const ProtocolSubReactor &) = delete;
    ProtocolSubReactor &operator=(const ProtocolSubReactor &) = delete;
    ProtocolSubReactor(ProtocolSubReactor &&) = delete;
    ProtocolSubReactor &operator=(ProtocolSubReactor &&) = delete;
    /**
     * @brief 启动一次，等待Sub内slot/epoll/eventfd/注册全部就绪；启动失败内部join
     * @return
     *     空表示曾完整就绪，随后仍可能运行失败；重复EALREADY，提前停止ECANCELED。
     *     系统失败原errno，thread system_error保留code，bad_alloc ENOMEM/其他EIO。
     * @note 控制同步和join异常可抛；启动失败不发布target，不需发布者屏障。
     */
    [[nodiscard]] std::error_code start();
    /**
     * @brief 控制者取得完整就绪目标的稳定借用；只用于该Main的专用入口
     * @return
     *     running时目标，否则ECANCELED；状态随后可变，不证明仍healthy。
     *     queue与fd须覆盖所有完整发布+通知/最后控制通知返回；release后禁止使用旧借用。
     * @note 本层不自动检测raw借用者，控制者须汇总所有Sub ready/首因。
     */
    [[nodiscard]] std::variant<MainReactorTarget, std::error_code> target();
    /**
     * @brief 控制者取得本Sub结果队列与同一eventfd的稳定发布借用
     * @return
     *     running时目标，否则ECANCELED；release后禁止使用旧借用。
     * @note
     *     控制者须等Main、submitter、worker及全部完整发布/通知调用返回后才release。
     *     立即停止会关闭入口并保留残留到静止后清理；排空期间继续应用，到期/活跃归零才关闭。
     */
    [[nodiscard]] std::variant<CompletionTarget, std::error_code> completion_target();
    /**
     * @brief 跨线程单调stop并有限通知，不关闭资源或等待发布者/线程
     * @return
     *     通知诊断；失败标志仍生效，撤销内部fd镜像后为空。
     * @note 同mutex串行内部通知与撤销，锁外close；mutex异常可抛，this仍须存活。
     */
    [[nodiscard]] std::error_code request_stop();
    /**
     * @brief 请求只推进已pending的结果/输出；重复只收紧绝对单调截止，不延长
     * @param[in] deadline
     *     正CLOCK_MONOTONIC纳秒时间点；已到期合法，0/负数EINVAL且状态保持。
     *
     * @return
     *     running/draining时发布并有限通知；其他状态ECANCELED。通知失败不撤回请求。
     * @note
     *     可跨线程调用，this须存活；观察到drain_started后最后read/submit回调已返回。
     *     调用返回本身不是提交静止屏障；控制者须先关闭任务准入并停止Main发布。
     *     空闲/半包关闭，在途排空或截止关闭，不读取缓存后续帧、不新增STOPPING响应。
     *     request_stop优先；结果与通知目标仍须存活到发布者静止后的release。
     * @throws
     *     控制mutex异常可抛；不拥有外部发布者，也不等待worker。
     */
    [[nodiscard]] std::error_code request_drain(std::int64_t deadline);
    /**
     * @brief 控制者承诺全部外部发布/通知已返回并撤销target，允许Sub最终清理，幂等
     * @note 必须先停止并等待Main、Sub提交回调、worker发布、最后控制通知与其他raw借用者返回。
     *     此方法同时置stop；不自动检测或等待借用者，不能在尚有发布者时调用。
     *     控制同步可抛；无通知系统调用，100ms等待后备仍有效。
     */
    void release_publishers();
    /**
     * @brief 外部join；不隐含release，ready后须先完成发布者屏障再调用
     * @return
     *     首因优先，否则清理诊断；重复保持，idle无操作，self-join EDEADLK。
     * @note 不持报告锁join；遗漏release会等待。标准join/同步错误可抛。
     */
    [[nodiscard]] std::error_code join();
    /** @brief 同步复制最近报告；quiescing首因在屏障前可见，mutex异常可抛。 */
    [[nodiscard]] ProtocolSubStatus status() const;
    /** @brief 可与生产/消费并发读两个队列自己的锁内深度；锁异常传播，不借用条目。 */
    [[nodiscard]] SubQueueDepths queue_depths() const;

private:
    void run() noexcept;
    const ProtocolSubConfig config_; ///< 构造预检后固定，Sub线程借用到join返回。
    FdHandoffQueue queue_; ///< 专用目标；原始目标借用保持到release，无跨Sub消费。
    concurrency::CompletionQueue completions_; ///< 固定owner邮箱，消费及残留销毁仅在Sub线程。
    std::thread thread_;            ///< 控制者串行拥有句柄，线程借用this。
    std::atomic<bool> stop_{false}; ///< 请求标志，不能代替发布者静止。
    std::atomic<std::int64_t> drain_deadline_{0}; ///< 0未请求；正绝对期限只收紧，所属线程执行关闭。
    mutable std::mutex mutex_; ///< 保护报告/启动/屏障/内部通知镜像，不保护Connection。
    std::condition_variable cv_;       ///< ready与发布者静止谓词等待，不持锁join。
    bool ready_done_ = false;          ///< 独立启动报告已完成，受mutex保护。
    std::error_code ready_error_;      ///< 保存启动结果，不被之后运行失败改写。
    bool publishers_released_ = false; ///< 控制者静止承诺，受mutex保护。
    int borrowed_wakeup_ = -1; ///< 内部stop通知镜像，Sub锁内撤销，锁外关闭真实拥有者。
    ProtocolSubStatus report_; ///< 生命周期/计数与诊断的最近快照，受mutex保护。
};
} // namespace siftwing::net
