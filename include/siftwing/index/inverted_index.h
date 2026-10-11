/*
 * PROJECT : SIFTWING
 * FILE    : inverted_index.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 从显式搜索保留集构建拥有型原始倒排、DF和文档统计
 */
#pragma once
#include "siftwing/index/lexicon.h"
#include "siftwing/index/posting.h"

namespace siftwing::index {
/** @brief 拥有一个显式选入搜索集合的完整单文档TF；不代表原文或去重证明。 */
struct IndexDocument final {
    base::DocumentId document_id; ///< 本批唯一强身份，0和UINT64_MAX合法。
    text::DocumentTermFrequencies frequencies; ///< 正次数表及精确总和；空表须总数0。
};
/** @brief 一个文档的值统计；空文档也有条目，不借用输入或索引。 */
struct DocumentStatistics final {
    base::DocumentId document_id; ///< 对应显式输入身份。
    std::uint64_t total_tokens = 0; ///< 过滤后的原始次数总和，不是关系数。
    std::uint64_t distinct_terms = 0; ///< 该文档的唯一词数，即它贡献的Posting数。
};
/** @brief 显式搜索集合的值统计，不从推荐累计或最大DocumentId推导。 */
struct CorpusStatistics final {
    std::uint64_t documents = 0; ///< N，包含显式选入的空文档。
    std::uint64_t total_tokens = 0; ///< 所有选入文档的次数总和。
    std::uint64_t total_postings = 0; ///< 所有唯一(词,文档)关系数，等于各词DF之和。
};
/** @brief 九项正预算，等于上限合法；不含输入装载/容器容量/排序开销，非硬资源限制。 */
struct InvertedIndexLimits final {
    LexiconLimits lexicon; ///< 五预算；输入词数/字节按跨文档的每个词键计，输出计并集。
    std::uint64_t max_documents = 0; ///< 输入文档条数，包括空文档。
    std::uint64_t max_document_tokens = 0; ///< 每篇受检TF总和。
    std::uint64_t max_total_tokens = 0; ///< 全库受检次数总和。
    std::uint64_t max_postings_per_term = 0; ///< 每个词的唯一文档关系数，即DF。
};
/**
 * @brief 拥有固定搜索Lexicon、按TermId对齐的只读倒排和按DocumentId递增的文档统计
 * @note 不自行去重；同词表/快照/资源兼容及输入保留集由调用方保证。TermId只属于本词典，
 *     不携带快照身份。DF由列表条数取得，TF完整保留uint64，不计算权重/IDF/归一化。
 * @note 不可复制，noexcept移动/赋值，源清空且统计归零，自移动保持。同步只读查询可独立
 *     执行；不能与所有者移动/赋值/析构并发。未实测并发，不含IO或共享可变缓存。
 */
class InvertedIndex final {
public:
    InvertedIndex(const InvertedIndex &) = delete;
    InvertedIndex &operator=(const InvertedIndex &) = delete;
    /** @brief 转交完整拥有状态，源成为空索引。 */
    InvertedIndex(InvertedIndex &&other) noexcept;
    /** @brief 释放原状态后转交，源清空，自移动保持。 */
    InvertedIndex &operator=(InvertedIndex &&other) noexcept;
    /**
     * @brief 验证完整TF后一次发布确定性倒排；输入排列不改变成功结果
     * @param[in] documents
     *     同步稳定借用，调用期间不得失效或并发写；拒重复ID，不排序/合并/写回输入。
     *     每项正次数、受检和须等于total_tokens；词键合同同Lexicon，无二次规范化/过滤。
     *     空批次成功空索引，空文档计入N但不贡献关系。
     * @param[in] limits
     *     先全正配置/文档数，再排序拒重复；按ID检查每篇TF和单篇/全库次数，再词条/字节
     *     预算；词键/唯一词表预算委托Lexicon，最后在关系插入前检查每词条数。
     *
     * @return
     *     完整拥有对象或index.limits/documents/doc_id/tf/overflow/document_tokens/
     *     total_tokens/postings_per_term/term诊断；词键与五词表预算沿用lexicon.*。
     *     失败无部分索引，输入保持。统计合法不能证明原文、去重历史或资源兼容。
     * @throws
     *     分配及未知异常上传，临时容器RAII释放，借用不逃逸。
     */
    [[nodiscard]] static base::Result<InvertedIndex> build(
        const std::vector<IndexDocument> &documents, const InvertedIndexLimits &limits);
    /** @brief 借用本索引词表；移动/赋值/析构失效，调用者不释放，不得并发修改所有者。 */
    [[nodiscard]] const Lexicon &lexicon() const & noexcept { return lexicon_; }
    const Lexicon &lexicon() const && = delete;
    /** @brief 借用按文档ID递增的只读统计；失效及同步条件同lexicon。 */
    [[nodiscard]] const std::vector<DocumentStatistics> &documents() const & noexcept { return documents_; }
    const std::vector<DocumentStatistics> &documents() const && = delete;
    /**
     * @brief 借用一个完整只读列表，不复制关系
     * @param[in] term
     *     本词表局部ID；未知ID返回nullptr，不验证跨词表误用。
     *
     * @return
     *     已知词的非空严格文档ID递增列表，失效及同步条件同lexicon，调用者不释放。
     */
    [[nodiscard]] const PostingList *postings(base::TermId term) const & noexcept;
    const PostingList *postings(base::TermId) const && = delete;
    /**
     * @brief 按文档ID二分借用统计
     * @param[in] document
     *     显式输入身份；未知ID返回nullptr。
     *
     * @return
     *     只读条目，失效及同步条件同lexicon，调用者不释放。
     */
    [[nodiscard]] const DocumentStatistics *document(base::DocumentId document) const & noexcept;
    const DocumentStatistics *document(base::DocumentId) const && = delete;
    /**
     * @brief 从唯一文档列表条数取得DF，不累计TF
     * @param[in] term
     *     本词表局部ID，未知ID返回nullopt。
     *
     * @return
     *     独立uint64值；已知词DF为正且不超过N，不借用索引。
     */
    [[nodiscard]] std::optional<std::uint64_t> document_frequency(base::TermId term) const noexcept;
    /** @brief 返回独立语料统计值，临时所有者销毁不影响返回值。 */
    [[nodiscard]] CorpusStatistics statistics() const noexcept { return statistics_; }
private:
    InvertedIndex(Lexicon lexicon, std::vector<PostingList> postings,
                  std::vector<DocumentStatistics> documents, CorpusStatistics statistics) noexcept;
    Lexicon lexicon_; ///< 拥有唯一搜索词表，词位置即TermId。
    std::vector<PostingList> postings_; ///< 与词表等长对齐，拥有完整正TF关系。
    std::vector<DocumentStatistics> documents_; ///< 拥有唯一递增文档统计，包含空文档。
    CorpusStatistics statistics_; ///< 与当前拥有状态一致的受检值统计。
};
} // namespace siftwing::index
