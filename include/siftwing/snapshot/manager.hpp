/*
 * PROJECT : SIFTWING
 * FILE    : manager.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 拥有当前只读快照、同步候选报告及显式原子发布入口
 */
#pragma once
#include "siftwing/snapshot/reader.hpp"

namespace siftwing::snapshot {
/** @brief 拥有完整只读Reader；副本延长所有部件的生命期，不借用Manager。 */
using SnapshotHandle = std::shared_ptr<const SnapshotReader>;

/** @brief 一次已完成加载的拥有报告；Manager返回的成功载荷始终非空且完整校验。 */
struct SnapshotLoadResult final {
    std::uint64_t requested_version; ///< 请求身份；失败时同样保留，不表示当前版本。
    base::Result<SnapshotHandle> outcome; ///< 拥有候选或原Reader诊断；异常不生成报告。
};
/** @brief 显式发布判定；不按版本数值排序，加载成功本身不触发发布。 */
enum class SnapshotPublishResult {
    published,        ///< 已替换current；已有读者/报告继续拥有旧Reader。
    unchanged,        ///< 候选与current是同一Reader对象，没有替换。
    invalid_candidate ///< 空句柄或移空Reader，current保持。
};

/**
 * @brief 管理当前快照及最后加载结果；仅显式发布才替换当前快照
 * @note current可与current、publish_candidate及load_candidate并发；发布由单控制者串行调用。
 *     last_load/load_candidate仍由控制者串行调用；Loader独占它们时也允许控制者发布。
 *     Manager在所有成员调用期间须稳定存活；析构不能与成员调用并发。
 *     已取得的句柄/报告可跨Manager析构独立只读；借用Reader部件须保留其句柄。
 *     不创建线程或限制进程内存，外部报告也可延长旧快照生命期；不承诺无锁/硬时限。
 */
class SnapshotManager final {
public:
    /** @brief 禁止复制管理状态；通过current取得共享只读所有权。 */
    SnapshotManager(const SnapshotManager &) = delete;
    /** @brief 禁止复制赋值。 */
    SnapshotManager &operator=(const SnapshotManager &) = delete;
    /** @brief 禁止移动，保持并发读者依赖的Manager地址和成员稳定。 */
    SnapshotManager(SnapshotManager &&) = delete;
    /** @brief 禁止移动赋值。 */
    SnapshotManager &operator=(SnapshotManager &&) = delete;
    /** @brief 释放自身拥有份额；外部句柄/报告仍有效。 */
    ~SnapshotManager() = default;

    /**
     * @brief 同步完整加载初始快照，成功后才返回Manager
     * @param[in] parent_path
     *     稳定可信快照父目录，路径/输入约束同SnapshotReader::open。
     * @param[in] version
     *     精确版本身份，含0和UINT64_MAX，不扫描最新版本。
     * @param[in] limits
     *     Reader完整校验的全正预算；不限制进程RSS。
     *
     * @return
     *     非空独占Manager，current与初始成功报告指向同一Reader；失败原样保留Reader诊断。
     * @throws
     *     分配及未知异常上传，局部资源由RAII释放，不返回部分Manager、不改写输入。
     */
    [[nodiscard]] static base::Result<std::unique_ptr<SnapshotManager>> open(
        const std::string &parent_path,
        base::SnapshotVersion version,
        const SnapshotReadLimits &limits);

    /** @brief 原子取得非空拥有句柄；不分配，跨发布仍拥有同一完整Reader。 */
    [[nodiscard]] SnapshotHandle current() const noexcept;

    /**
     * @brief 原子发布完整只读Reader；拒空/移空，不分配、不访问文件或修改last_load
     * @param[in] candidate
     *     按值取得共享所有权；须来自真实Reader成功加载，禁止通过可变别名移动或修改其对象。
     *
     * @return
     *     published/unchanged/invalid_candidate；拒绝不改current。相同版本的不同Reader也可发布。
     * @note
     *     版本是不透明身份，允许0、max及回滚。发布后才取得句柄的读者见此或更后发布；
     *     已取得句柄继续旧Reader。原子交换锁外释放旧份额，最后拥有者决定实际回收时点。
     */
    [[nodiscard]] SnapshotPublishResult publish_candidate(SnapshotHandle candidate) noexcept;

    /** @brief 返回非空拥有报告；与load_candidate串行，后续替换不使已有报告失效。 */
    [[nodiscard]] std::shared_ptr<const SnapshotLoadResult> last_load() const noexcept;

    /**
     * @brief 同步完整校验候选并保存本次报告，成功和普通失败均不改变current
     * @param[in] parent_path
     *     稳定可信父目录，约束同Reader::open；调用线程承担全部文件I/O与解析。
     * @param[in] version
     *     候选精确版本，可与当前相同；不强制版本单调递增。
     * @param[in] limits
     *     本次Reader全正预算，不继承初始加载配置。
     *
     * @return
     *     与last_load相同的非空拥有报告；成功候选完整非空，普通失败诊断原样保留。
     * @throws
     *     分配及未知异常上传，current和原last_load保持；临时Reader由RAII释放。
     * @note
     *     与last_load及其他load_candidate串行；允许current并发，不创建线程或发布候选。
     */
    [[nodiscard]] std::shared_ptr<const SnapshotLoadResult> load_candidate(
        const std::string &parent_path,
        base::SnapshotVersion version,
        const SnapshotReadLimits &limits);

private:
    SnapshotManager(
        SnapshotHandle current, std::shared_ptr<const SnapshotLoadResult> report) noexcept;
    SnapshotHandle current_; ///< 完整Reader；构造后仅通过shared_ptr原子自由函数访问。
    std::shared_ptr<const SnapshotLoadResult> last_load_; ///< 控制者串行替换，报告自身只读拥有。
};
} // namespace siftwing::snapshot
