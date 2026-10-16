/*
 * PROJECT : SIFTWING
 * FILE    : manager.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 在完整加载及拥有报告构造后提交最后结果，保持当前快照不变
 */
#include "siftwing/snapshot/manager.hpp"
#include <utility>

namespace siftwing::snapshot {
namespace {
base::Result<SnapshotHandle> load(
    const std::string &parent, base::SnapshotVersion version, const SnapshotReadLimits &limits) {
    auto reader = SnapshotReader::open(parent, version, limits);
    if (!reader)
        return base::Result<SnapshotHandle>::failure(std::move(reader).error());
    return base::Result<SnapshotHandle>::success(
        std::make_shared<const SnapshotReader>(std::move(reader).value()));
}
} // namespace

SnapshotManager::SnapshotManager(
    SnapshotHandle current, std::shared_ptr<const SnapshotLoadResult> report) noexcept
    : current_(std::move(current)), last_load_(std::move(report)) {}

base::Result<std::unique_ptr<SnapshotManager>> SnapshotManager::open(const std::string &parent_path,
    base::SnapshotVersion version,
    const SnapshotReadLimits &limits) {
    using Output = base::Result<std::unique_ptr<SnapshotManager>>;
    auto loaded = load(parent_path, version, limits);
    if (!loaded)
        return Output::failure(std::move(loaded).error());
    auto current = loaded.value();
    auto report = std::make_shared<const SnapshotLoadResult>(
        SnapshotLoadResult{version.value(), std::move(loaded)});
    return Output::success(std::unique_ptr<SnapshotManager>(
        new SnapshotManager(std::move(current), std::move(report))));
}

SnapshotHandle SnapshotManager::current() const noexcept {
    return current_;
}

std::shared_ptr<const SnapshotLoadResult> SnapshotManager::last_load() const noexcept {
    return last_load_;
}

std::shared_ptr<const SnapshotLoadResult> SnapshotManager::load_candidate(
    const std::string &parent_path,
    base::SnapshotVersion version,
    const SnapshotReadLimits &limits) {
    // 分配和完整Reader校验全部发生在替换前；最后共享指针赋值不抛异常。
    auto report = std::make_shared<const SnapshotLoadResult>(
        SnapshotLoadResult{version.value(), load(parent_path, version, limits)});
    last_load_ = report;
    return report;
}
} // namespace siftwing::snapshot
