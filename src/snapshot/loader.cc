/*
 * PROJECT : SIFTWING
 * FILE    : loader.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-16
 * BRIEF   : 锁内交接单槽状态，锁外完整加载，异常作为拥有完成值交付
 */
#include "siftwing/snapshot/loader.hpp"
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace siftwing::snapshot {
static_assert(std::is_nothrow_move_constructible_v<SnapshotLoadRequest>);
static_assert(std::is_nothrow_move_constructible_v<SnapshotLoadCompletion>);

SnapshotLoader::SnapshotLoader(SnapshotManager &manager)
    : manager_(manager), thread_([this] {
          worker_main();
      }) {}

SnapshotLoader::~SnapshotLoader() noexcept {
    try {
        join();
    } catch (...) {
        std::terminate();
    }
}

SnapshotLoadSubmit SnapshotLoader::try_submit(SnapshotLoadRequest request) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return SnapshotLoadSubmit::stopped;
        if (occupied_)
            return SnapshotLoadSubmit::busy;
        pending_.emplace(std::move(request));
        occupied_ = true;
    }
    wake_.notify_one();
    return SnapshotLoadSubmit::accepted;
}

std::optional<SnapshotLoadCompletion> SnapshotLoader::take_completed() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!completed_)
        return std::nullopt;
    auto result = std::move(completed_);
    completed_.reset();
    occupied_ = false;
    return result;
}

void SnapshotLoader::request_stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    wake_.notify_one();
}

void SnapshotLoader::join() {
    if (thread_.get_id() == std::this_thread::get_id())
        throw std::logic_error("snapshot loader cannot join itself");
    request_stop();
    if (thread_.joinable())
        thread_.join();
}

void SnapshotLoader::worker_main() noexcept {
    try {
        for (;;) {
            std::optional<SnapshotLoadRequest> request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] {
                    return stopped_ || pending_.has_value();
                });
                if (!pending_)
                    return;
                request.emplace(std::move(*pending_));
                pending_.reset();
            }
            SnapshotLoadCompletion result{request->version.value(), nullptr, nullptr};
            try {
                // Manager保持原串行控制合同；加载和旧报告释放均不持Loader状态锁。
                result.report = manager_.load_candidate(
                    request->parent_path, request->version, request->limits);
            } catch (...) {
                result.exception = std::current_exception();
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                completed_.emplace(std::move(result));
            }
            // occupied直到消费者取走才解除，完成后不能覆盖或累积更多候选。
        }
    } catch (...) {
        std::terminate();
    }
}
} // namespace siftwing::snapshot
