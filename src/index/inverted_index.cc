/*
 * PROJECT : SIFTWING
 * FILE    : inverted_index.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 校验显式TF集合后建立确定性词到文档关系
 * IMPLEMENTATION : 借用文档排序不写回，受检统计先验；局部拥有对象仅在完整成功后发布
 */
#include "siftwing/index/inverted_index.hpp"
#include <algorithm>
#include <utility>

namespace siftwing::index {
namespace {
template <typename T>
base::Result<T> fail(const char *context, const char *message) {
    return base::Result<T>::failure({message, context});
}
bool valid_limits(const InvertedIndexLimits &v) {
    const auto &l = v.lexicon;
    return l.max_input_terms && l.max_input_bytes && l.max_term_bytes && l.max_distinct_terms
           && l.max_vocabulary_bytes && v.max_documents && v.max_document_tokens
           && v.max_total_tokens && v.max_postings_per_term;
}
} // namespace
InvertedIndex::InvertedIndex(Lexicon lexicon,
    std::vector<PostingList> postings,
    std::vector<DocumentStatistics> documents,
    CorpusStatistics statistics) noexcept
    : lexicon_(std::move(lexicon)), postings_(std::move(postings)),
      documents_(std::move(documents)), statistics_(statistics) {}
InvertedIndex::InvertedIndex(InvertedIndex &&other) noexcept
    : lexicon_(std::move(other.lexicon_)), postings_(std::move(other.postings_)),
      documents_(std::move(other.documents_)), statistics_(other.statistics_) {
    other.postings_.clear();
    other.documents_.clear();
    other.statistics_ = {};
}
InvertedIndex &InvertedIndex::operator=(InvertedIndex &&other) noexcept {
    if (this != &other) {
        lexicon_ = std::move(other.lexicon_);
        postings_ = std::move(other.postings_);
        documents_ = std::move(other.documents_);
        statistics_ = other.statistics_;
        other.postings_.clear();
        other.documents_.clear();
        other.statistics_ = {};
    }
    return *this;
}
base::Result<InvertedIndex> InvertedIndex::build(
    const std::vector<IndexDocument> &input, const InvertedIndexLimits &limits) {
    using Output = base::Result<InvertedIndex>;
    if (!valid_limits(limits)) {
        return fail<InvertedIndex>("index.limits", "nine positive limits required");
    }
    if (input.size() > limits.max_documents) {
        return fail<InvertedIndex>("index.documents", "too many documents");
    }
    std::vector<const IndexDocument *> sorted;
    sorted.reserve(input.size());
    for (const auto &entry : input) {
        sorted.push_back(&entry);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto *a, const auto *b) {
        return a->document_id < b->document_id;
    });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i - 1]->document_id == sorted[i]->document_id) {
            return fail<InvertedIndex>("index.doc_id", "duplicate document ID");
        }
    }
    CorpusStatistics statistics;
    const auto count = base::checked_narrow<std::uint64_t>(sorted.size(), "index.overflow");
    if (!count) {
        return Output::failure(count.error());
    }
    statistics.documents = count.value();
    std::vector<DocumentStatistics> documents;
    documents.reserve(sorted.size());
    std::vector<std::string_view> keys;
    std::uint64_t input_bytes = 0;
    for (const auto *entry : sorted) {
        const auto &tf = entry->frequencies;
        std::uint64_t total = 0;
        for (const auto &term : tf.terms) {
            if (term.second == 0) {
                return fail<InvertedIndex>("index.tf", "term frequency must be positive");
            }
            const auto sum = base::checked_add(total, term.second, "index.overflow");
            if (!sum) {
                return Output::failure(sum.error());
            }
            total = sum.value();
        }
        if (total != tf.total_tokens) {
            return fail<InvertedIndex>("index.tf", "document total does not match frequencies");
        }
        if (total > limits.max_document_tokens) {
            return fail<InvertedIndex>("index.document_tokens", "document token budget exceeded");
        }
        const auto sum = base::checked_add(statistics.total_tokens, total, "index.overflow");
        if (!sum) {
            return Output::failure(sum.error());
        }
        if (sum.value() > limits.max_total_tokens) {
            return fail<InvertedIndex>("index.total_tokens", "corpus token budget exceeded");
        }
        statistics.total_tokens = sum.value();
        const auto distinct =
            base::checked_narrow<std::uint64_t>(tf.terms.size(), "index.overflow");
        if (!distinct) {
            return Output::failure(distinct.error());
        }
        const auto relations =
            base::checked_add(statistics.total_postings, distinct.value(), "index.overflow");
        if (!relations) {
            return Output::failure(relations.error());
        }
        if (relations.value() > limits.lexicon.max_input_terms) {
            return fail<InvertedIndex>("lexicon.input_terms", "too many input terms");
        }
        statistics.total_postings = relations.value();
        documents.push_back({entry->document_id, total, distinct.value()});
        // 词表仍验证全部词键；在借用键容器增长前先限制条数和字节，不提前复制输入词。
        for (const auto &term : tf.terms) {
            if (term.first.size() > limits.lexicon.max_term_bytes) {
                return fail<InvertedIndex>("lexicon.term_bytes", "term exceeds byte budget");
            }
            if (term.first.size() > limits.lexicon.max_input_bytes - input_bytes) {
                return fail<InvertedIndex>("lexicon.input_bytes", "input bytes exceed budget");
            }
            input_bytes += term.first.size();
            keys.push_back(term.first);
        }
    }
    auto lexicon = Lexicon::build(keys, limits.lexicon);
    if (!lexicon) {
        return Output::failure(lexicon.error());
    }
    std::vector<PostingList> postings(lexicon.value().terms().size());
    for (const auto *entry : sorted) {
        for (const auto &term : entry->frequencies.terms) {
            const auto id = lexicon.value().find(term.first);
            if (!id) {
                return fail<InvertedIndex>(
                    "index.term", "term missing from constructed vocabulary");
            }
            auto &list = postings[static_cast<std::size_t>(id->value())];
            if (list.size() >= limits.max_postings_per_term) {
                return fail<InvertedIndex>(
                    "index.postings_per_term", "term document budget exceeded");
            }
            // 每篇map键唯一，文档先排序并拒重复，因此每词自然生成严格递增唯一列表。
            auto posting = Posting::from_count(entry->document_id, term.second);
            if (!posting) {
                return Output::failure(posting.error());
            }
            list.push_back(posting.value());
        }
    }
    return Output::success(InvertedIndex{
        std::move(lexicon).value(), std::move(postings), std::move(documents), statistics});
}
const PostingList *InvertedIndex::postings(base::TermId term) const & noexcept {
    if (term.value() >= postings_.size()) {
        return nullptr;
    }
    return &postings_[static_cast<std::size_t>(term.value())];
}
const DocumentStatistics *InvertedIndex::document(base::DocumentId id) const & noexcept {
    const auto it = std::lower_bound(
        documents_.begin(), documents_.end(), id, [](const auto &entry, base::DocumentId key) {
            return entry.document_id < key;
        });
    return it != documents_.end() && it->document_id == id ? &*it : nullptr;
}
std::optional<std::uint64_t> InvertedIndex::document_frequency(base::TermId term) const noexcept {
    const auto *list = postings(term);
    if (!list) {
        return std::nullopt;
    }
    // 构建已将所有关系数限制在uint64词条预算内，不缓存与列表可能失配的第二份DF。
    return static_cast<std::uint64_t>(list->size());
}
} // namespace siftwing::index
