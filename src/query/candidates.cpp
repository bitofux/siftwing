/*
 * PROJECT : SIFTWING
 * FILE    : candidates.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 以最短列表探测交集、最小堆合并并集，完整成功后发布
 * IMPLEMENTATION : 先准入全部已知列表；AND只存最终命中，OR每列表保留一个游标。
 */
#include "siftwing/query/candidates.h"
#include <algorithm>
#include <utility>

namespace siftwing::query {
namespace {
using Output = base::Result<CandidateDocuments>;
Output fail(const char *context, const char *message) {
    return Output::failure({message, context});
}
struct Cursor final {
    const index::PostingList *list; // 只在同步合并期间借用有效索引列表。
    std::size_t position;
};
bool later(const Cursor &a, const Cursor &b) {
    return (*b.list)[b.position].document_id() < (*a.list)[a.position].document_id();
}
} // namespace

base::Result<CandidateDocuments> collect_candidates(
    const Query &query, const index::InvertedIndex &index, const CandidateLimits &limits) {
    if (!limits.max_query_terms || !limits.max_postings || !limits.max_candidates) {
        return fail("candidate.limits", "候选三项预算必须为正");
    }
    if (query.empty()) { return fail("candidate.query", "移空查询不能参与匹配"); }
    if (query.frequencies().terms.size() > limits.max_query_terms) {
        return fail("candidate.terms", "查询唯一词数超出预算");
    }
    std::vector<const index::PostingList *> lists;
    bool unknown = false;
    std::uint64_t admitted = 0;
    for (const auto &term : query.frequencies().terms) {
        const auto id = index.lexicon().find(term.first);
        if (!id) { unknown = true; continue; }
        const auto *list = index.postings(*id);
        // 有效索引保证已知词有非空列表；减法先比较，避免条数相加回绕。
        if (list->size() > limits.max_postings - admitted) {
            return fail("candidate.postings", "已知词完整列表总条数超出准入预算");
        }
        admitted += static_cast<std::uint64_t>(list->size());
        lists.push_back(list);
    }
    CandidateDocuments documents;
    if (lists.empty() || (query.mode() == QueryMode::all && unknown)) {
        return Output::success(std::move(documents));
    }
    if (query.mode() == QueryMode::all) {
        // 最短列表是交集的超集。逐候选二分探测其余列表，不产生可能大于最终交集的临时输出。
        const auto shortest = std::min_element(lists.begin(), lists.end(),
            [](const auto *a, const auto *b) { return a->size() < b->size(); });
        for (const auto &posting : **shortest) {
            const auto document = posting.document_id();
            bool matches = true;
            for (const auto *list : lists) {
                if (list == *shortest) { continue; }
                const auto it = std::lower_bound(list->begin(), list->end(), document,
                    [](const auto &item, base::DocumentId key) { return item.document_id() < key; });
                if (it == list->end() || it->document_id() != document) { matches = false; break; }
            }
            if (!matches) { continue; }
            if (documents.size() >= limits.max_candidates) {
                return fail("candidate.documents", "最终候选数超出预算");
            }
            documents.push_back(document);
        }
    } else {
        // 每个列表只保留一个未消费首项。最小堆产生递增流，相邻相等身份只发布一次。
        std::vector<Cursor> heap;
        for (const auto *list : lists) { heap.push_back({list, 0}); }
        std::make_heap(heap.begin(), heap.end(), later);
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end(), later);
            auto cursor = heap.back(); heap.pop_back();
            const auto document = (*cursor.list)[cursor.position].document_id();
            if (documents.empty() || documents.back() != document) {
                if (documents.size() >= limits.max_candidates) {
                    return fail("candidate.documents", "最终候选数超出预算");
                }
                documents.push_back(document);
            }
            ++cursor.position;
            if (cursor.position < cursor.list->size()) {
                heap.push_back(cursor);
                std::push_heap(heap.begin(), heap.end(), later);
            }
        }
    }
    return Output::success(std::move(documents));
}
} // namespace siftwing::query
