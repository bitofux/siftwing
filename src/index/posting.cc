/*
 * PROJECT : SIFTWING
 * FILE    : posting.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 在预算内校验原始Posting列表的严格文档次序，保持输入不变
 */
#include "siftwing/index/posting.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace siftwing::index {
base::Result<void> validate_posting_list(
    const PostingList &postings, const PostingListLimits &limits) {
    if (limits.max_postings == 0) {
        return base::Result<void>::failure({"posting limit must be positive", "posting.limits"});
    }
    auto size = base::checked_narrow<std::uint64_t>(postings.size(), "posting.size");
    if (!size) {
        return base::Result<void>::failure(std::move(size).error());
    }
    if (size.value() > limits.max_postings) {
        return base::Result<void>::failure({"posting list exceeds count limit", "posting.size"});
    }
    // 直接比较强ID而非相减；0和UINT64_MAX不被当成哨兵，也不引入加减回绕。
    // 严格小于同时排除逆序和重复；不合并重复TF，以免掩盖重复文档交付。
    for (std::size_t i = 1; i < postings.size(); ++i) {
        if (!(postings[i - 1].document_id() < postings[i].document_id())) {
            return base::Result<void>::failure(
                {"document IDs must increase strictly at posting " + std::to_string(i),
                    "posting.order"});
        }
    }
    return base::Result<void>::success();
}
} // namespace siftwing::index
