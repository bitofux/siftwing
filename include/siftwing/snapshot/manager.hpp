/*
 * PROJECT : SIFTWING
 * FILE    : manager.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 拥有当前只读快照及同步候选加载结果，尚不发布候选
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

/**
 * @brief 管理固定当前快照及最后一次同步加载结果；候选成功也不替换当前快照
 * @note current可与current及load_candidate并发；last_load/load_candidate由控制者串行调用。
 *     Manager在所有成员调用期间须稳定存活；析构不能与成员调用并发。
 *     已取得的句柄/报告可跨Manager析构独立只读；借用Reader部件须保留其句柄。
 *     没有后台线程、发布接口或进程内存硬上限，外部报告可延长旧候选生命期。
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

    /** @brief 返回非空拥有句柄；不分配，不改变当前版本，可并发读取。 */
    [[nodiscard]] SnapshotHandle current() const noexcept;

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
    const SnapshotHandle current_; ///< 初始完整Reader；构造后不再写，支持并发复制句柄。
    std::shared_ptr<const SnapshotLoadResult> last_load_; ///< 控制者串行替换，报告自身只读拥有。
};
} // namespace siftwing::snapshot
