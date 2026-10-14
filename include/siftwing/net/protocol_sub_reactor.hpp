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
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace siftwing::net {
/** @brief 启动后固定的容量/预算；不约束全局RSS、JSON CPU或调度延迟。 */
struct ProtocolSubConfig {
    std::size_t handoff_capacity = 128; ///< [1,1024]，专用queue条目数。
    std::size_t max_connections = 128; ///< [1,1024]，Sub内固定slot数，满时在Sub关闭新源。
    std::size_t max_body_bytes = frame_max_body_bytes; ///< [2,frame_max_body_bytes]，每Buffer上限。
    std::size_t handoff_budget = 16; ///< [1,1024]，每轮最多pop的条目数，不按通知计数pop。
    std::size_t read_byte_budget = 65536; ///< [1,24+frame_max_body_bytes]，每连接每轮recv字节。
    std::size_t write_byte_budget = 65536; ///< 同上，send字节。
    std::size_t io_attempt_budget = 16;    ///< [1,1024]，read/flush各自的尝试预算。
    std::uint32_t reactor_id =
        0; ///< 本地身份命名空间；0保留旧默认行为，控制者保证并存目标唯一且旧结果存活时不重用。
};
/** @brief quiescing已停止业务但保留资源等发布者；stopped已清理，joined已回收线程。 */
enum class ProtocolSubState {
    idle,
    starting,
    running,
    quiescing,
    stopped,
    joined
};
/** @brief 同一报告锁的按值观测，不授予连接借用，也不保证下一刻仍运行。 */
struct ProtocolSubStatus {
    ProtocolSubState state = ProtocolSubState::idle; ///< 生命周期，禁止从quiescing推断可销毁。
    bool stop_requested = false; ///< 停止请求，不等于线程退出或响应完成。
    std::size_t received = 0;    ///< 正常调度从queue取得的条目，含注册前拒绝。
    std::size_t admitted = 0;  ///< 曾成功构造并ADD的连接数，归属本Sub直到终止。
    std::size_t rejected = 0;  ///< received中未注册的源，在Sub关闭。
    std::size_t dropped = 0;   ///< 曾注册后释放的连接数，含停止时丢弃。
    std::size_t discarded = 0; ///< 静止后清理尚在queue的条目，不计received。
    std::size_t active = 0;    ///< 最近发布的活跃连接数，最终为0。
    std::size_t requests = 0;  ///< 完整受信请求数，含Body拒绝或未接业务的类型。
    std::size_t responses = 0; ///< 输出排空且finish成功，不证明对端收到。
    std::error_code first_error;            ///< 启动/运行首因，正常停止为空。
    std::error_code cleanup_error;          ///< 首个独立清理诊断，不覆盖运行首因。
    std::error_code first_connection_error; ///< 局部客户端错误，不自动终止Sub。
};
/**
 * @brief 拥有专用queue与一个协议I/O线程，不监听，不accept，不访问其他Sub连接
 * @note 构造/start/target/release_publishers/join/析构由唯一外部控制者串行调用。
 *     request_stop/status可并发，但this须存活，析构前撤入口并等调用返回。不可copy/move。
 *     只响应PING/PONG与受信ERROR，搜索/worker未接入；停止立即丢弃，不保证响应排空。
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
     * @brief 跨线程单调stop并有限通知，不关闭资源或等待发布者/线程
     * @return
     *     通知诊断；失败标志仍生效，撤销内部fd镜像后为空。
     * @note 同mutex串行内部通知与撤销，锁外close；mutex异常可抛，this仍须存活。
     */
    [[nodiscard]] std::error_code request_stop();
    /**
     * @brief 控制者承诺全部外部发布/通知已返回并撤销target，允许Sub最终清理，幂等
     * @note 必须先停止并等待Main所有调用、最后控制通知与其他raw借用者返回。
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

private:
    void run() noexcept;
    const ProtocolSubConfig config_; ///< 构造预检后固定，Sub线程借用到join返回。
    FdHandoffQueue queue_; ///< 专用目标；原始目标借用保持到release，无跨Sub消费。
    std::thread thread_;   ///< 控制者串行拥有句柄，线程借用this。
    std::atomic<bool> stop_{false}; ///< 请求标志，不能代替发布者静止。
    mutable std::mutex mutex_; ///< 保护报告/启动/屏障/内部通知镜像，不保护Connection。
    std::condition_variable cv_;       ///< ready与发布者静止谓词等待，不持锁join。
    bool ready_done_ = false;          ///< 独立启动报告已完成，受mutex保护。
    std::error_code ready_error_;      ///< 保存启动结果，不被之后运行失败改写。
    bool publishers_released_ = false; ///< 控制者静止承诺，受mutex保护。
    int borrowed_wakeup_ = -1; ///< 内部stop通知镜像，Sub锁内撤销，锁外关闭真实拥有者。
    ProtocolSubStatus report_; ///< 生命周期/计数与诊断的最近快照，受mutex保护。
};
} // namespace siftwing::net
