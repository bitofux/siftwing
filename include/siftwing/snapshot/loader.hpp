/*
 * PROJECT : SIFTWING
 * FILE    : loader.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 用单线程和单任务槽异步加载完整候选，保留当前快照
 */
#pragma once
#include "siftwing/snapshot/manager.hpp"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

namespace siftwing::snapshot {
/** @brief 拥有一次加载的全部输入；文件须在实际后台加载期间保持可信稳定。 */
struct SnapshotLoadRequest final {
    std::string parent_path; ///< 拥有父目录路径，不借用提交者字符串；约束沿Reader。
    base::SnapshotVersion version; ///< 精确版本，0/max/相同/回退合法，不扫描最新版本。
    SnapshotReadLimits limits; ///< 本次完整校验的独立正预算，不是RSS硬上限。
};
/** @brief 提交判定；关闭优先于忙，拒绝不排队、不替换已有任务或结果。 */
enum class SnapshotLoadSubmit {
    accepted, ///< 已交接，后台执行一次；可能在提交返回前完成。
    busy,     ///< 唯一槽被等待、执行或尚未取走的结果占用。
    stopped   ///< 已关闭，不可重启。
};
/** @brief 一次已完成异步尝试；Loader交付值中report与exception恰有一个非空。 */
struct SnapshotLoadCompletion final {
    std::uint64_t requested_version; ///< 包括异常路径的请求身份，不表示当前版本。
    std::shared_ptr<const SnapshotLoadResult> report; ///< 普通成功/失败的原Manager拥有报告。
    std::exception_ptr exception; ///< 加载抛出的原异常，调用者可重抛；不伪装成Reader错误。
};
/**
 * @brief 专用线程串行完整加载候选；一个槽覆盖等待、执行和未消费结果，绝不发布
 * @note 构造前结束Manager的last_load/load_candidate操作；构造至join返回独占这两个入口。
 *     此期间外部可并发读取current，单控制者可显式publish_candidate；不可另建Loader共用Manager。
 *     Manager须稳定存活且晚于本对象销毁；已取走报告可跨两者析构独立存活。
 *     submit/take/stop可并发，均可短暂等待mutex，锁不包文件I/O，不承诺无锁/硬时限。
 *     join只由一个非加载线程控制者串行调用；销毁前撤销并等待全部外部调用。
 *     加载异常保存后继续服务下一任务；worker同步设施失效及析构回收失败terminate。
 */
class SnapshotLoader final {
public:
    /**
     * @brief 借用Manager并启动一个空闲等待线程，成功后可提交
     * @param[in,out] manager
     *     非空稳定对象；控制入口独占和生命期约束见类合同。
     *
     * @throws
     *     同步对象/线程创建异常上传；未返回对象不接受任务，也不改变Manager。
     */
    explicit SnapshotLoader(SnapshotManager &manager);
    /** @brief 自动stop/drain/join；正在执行的加载不可取消，阻塞I/O可使析构无限等待。 */
    ~SnapshotLoader() noexcept;
    /** @brief 禁止复制线程与其借用控制权。 */
    SnapshotLoader(const SnapshotLoader &) = delete;
    /** @brief 禁止复制赋值。 */
    SnapshotLoader &operator=(const SnapshotLoader &) = delete;
    /** @brief 禁止移动，后台线程捕获稳定this。 */
    SnapshotLoader(SnapshotLoader &&) = delete;
    /** @brief 禁止移动赋值。 */
    SnapshotLoader &operator=(SnapshotLoader &&) = delete;
    /**
     * @brief 尝试交接拥有输入，不等空位或加载完成；拒绝无任务副作用
     * @param[in] request
     *     按值取得输入，含调用前复制/移动；被拒绝的局部输入仍会销毁。
     *
     * @return
     *     stopped优先、其次busy、否则accepted；预算与路径在后台由Reader验证。
     * @throws
     *     参数构造/加锁失败上传，尚未接受任务；接受后的交接与通知不分配。
     */
    [[nodiscard]] SnapshotLoadSubmit try_submit(SnapshotLoadRequest request);
    /**
     * @brief 非等待地取走唯一完成值并释放槽；未完成为空，关闭/join后仍可取
     * @return
     *     optional拥有值，普通失败报告或异常都须处理；不重抛加载异常。
     * @throws
     *     加锁失败上传，结果保持；取走与移交不分配。
     */
    [[nodiscard]] std::optional<SnapshotLoadCompletion> take_completed();
    /** @brief 幂等关闭并唤醒；不取消已接受任务、不等待线程，锁失败上传。 */
    void request_stop();
    /**
     * @brief 先stop再排空并join；可重复，返回后可恢复Manager串行控制
     * @throws
     *     self-join抛logic_error且不关闭入口；控制者锁/join系统错误上传，可再尝试回收。
     * @note
     *     仅一个控制者串行join，不等待外部submit/take/stop调用结束，没有取消/硬截止。
     */
    void join();

private:
    void worker_main() noexcept;
    SnapshotManager &manager_; ///< 借用至析构；本线程独占其last/load入口。
    std::mutex mutex_;         ///< 保护以下全部状态，不包Reader校验或候选销毁。
    std::condition_variable wake_; ///< 空闲时睡眠，仅由提交或停止唤醒，谓词防虚假唤醒。
    bool stopped_ = false; ///< 锁内单向关闭，关闭后仍完成已接收任务。
    bool occupied_ = false; ///< 从接受到完成取走恒真，最多一个拥有任务/完成结果。
    std::optional<SnapshotLoadRequest> pending_;      ///< 锁内移交给后台的拥有输入。
    std::optional<SnapshotLoadCompletion> completed_; ///< 锁内发布，外部取走前不覆盖。
    std::thread thread_; ///< 最后构造，之前所有后台可见字段已完成初始化。
};
} // namespace siftwing::snapshot
