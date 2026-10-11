/*
 * PROJECT : SIFTWING
 * FILE    : weighted_index.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 定义平滑TF-IDF、文档L2权重及确定性字节导出
 */
#pragma once
#include "siftwing/index/inverted_index.h"

namespace siftwing::index {
/** @brief TF为原始次数，IDF=1+ln((N+1)/(DF+1))，每篇L2归一化；非快照版本。 */
inline constexpr std::uint32_t weight_policy_version = 1;
/** @brief 当前规范文本导出版本，不定义完整快照目录或发布协议。 */
inline constexpr std::uint32_t weight_serialization_version = 1;
/** @brief 三项正预算，含端点；不计已有输入装载、容器容量和分配器开销。 */
struct WeightLimits final {
    std::uint64_t max_documents = 0; ///< 原始索引N，包括显式空文档。
    std::uint64_t max_terms = 0; ///< 固定词表条数。
    std::uint64_t max_postings = 0; ///< 全库词-文档关系数，不是总出现次数。
};
/** @brief 正字节预算；限制成功字符串长度，不是进程硬内存限制。 */
struct WeightSerializationLimits final {
    std::uint64_t max_bytes = 0; ///< 完整导出文本长度，等于上限合法。
};
/**
 * @brief 平滑IDF值，供建库及后续同公式消费者复用
 * @param[in] documents
 *     已为uint64的搜索N；须为正，不从推荐交付数或最大DocId推导。
 * @param[in] frequency
 *     已为uint64的DF，须1<=DF<=N；检查不能追回调用前截断或转换。
 *
 * @return
 *     有限且>=1的double，或weights.statistics/rounding/numeric诊断。
 *     先统计合法性，再要求FE_TONEAREST；N/DF+1不作整数加法，完整uint64范围合法。
 * @throws
 *     诊断分配及未知异常上传。uint64转double可能舍入，原始TF/统计不因此修改。
 */
[[nodiscard]] base::Result<double> smoothed_idf(std::uint64_t documents, std::uint64_t frequency);
/**
 * @brief 拥有原始索引与按其词/关系位置对齐的归一化权重
 * @note build显式接管InvertedIndex；失败/异常也不归还已经交付的输入。无原始索引复制，
 *     原始TF保持整数，权重单独拥有。接管不改变词ID、关系次序、DF/N或空文档统计。
 * @note move-only/noexcept移动清源，自移动保持；所有借用在移动/赋值/析构后失效。
 *     同步只读可独立执行，但不能与所有者移动/赋值/析构并发，未实测并发。
 * @note 要求IEEE754 binary64、double与uint64位表示兼容、FE_TONEAREST、无fast-math。
 *     同环境/输入/策略按固定TermId次序计算，跨架构/libm版本不承诺逐位一致。
 */
class WeightedIndex final {
public:
    WeightedIndex(const WeightedIndex &) = delete;
    WeightedIndex &operator=(const WeightedIndex &) = delete;
    /** @brief 转交完整拥有状态，源成为空原始索引和空权重。 */
    WeightedIndex(WeightedIndex &&other) noexcept;
    /** @brief 释放原状态后转交，自移动保持，源清空。 */
    WeightedIndex &operator=(WeightedIndex &&other) noexcept;
    /**
     * @brief 接管有效原始索引，计算IDF、L2范数及逐关系归一化权重，一次发布
     * @param[in] input
     *     按值接管，调用方须显式移动；原始索引已验证、不再去重/规范化。
     *     空批次/全空文档成功，空篇范数+0且没有权重占位；其他篇范数正。
     * @param[in] limits
     *     先全正配置、浮点环境，再documents/terms/postings预算，最后计算与数值检查。
     *
     * @return
     *     完整拥有对象或weights.limits/rounding/platform/documents/terms/postings/numeric
     *     诊断，失败无部分加权对象。局部输入及容器RAII释放。
     * @throws
     *     分配及未知异常上传，已接管的输入不归还。
     * @note TF用double(raw_count)，w=TF*IDF；按TermId递增用hypot累计每篇范数，最后w/norm。
     *     不使用相对TF（同篇公共缩放会在L2中抵消），不裁剪词或改写原始计数。
     */
    [[nodiscard]] static base::Result<WeightedIndex> build(InvertedIndex input, const WeightLimits &limits);
    /** @brief 借用只读拥有原始索引；所有者移动/赋值/析构失效，不释放、不并发改所有者。 */
    [[nodiscard]] const InvertedIndex &raw_index() const & noexcept { return raw_; }
    const InvertedIndex &raw_index() const && = delete;
    /**
     * @brief 借用一个词的只读归一化权重数组，未知词返回nullptr
     * @param[in] term
     *     本索引局部词ID；数组与raw_index().postings(term)等长、逐位置对应。
     *
     * @return
     *     已知词的非空正有限权重数组，每项<=1；失效及同步条件同raw_index。
     *     不检查跨词表误用；不能把位置当文档ID。
     */
    [[nodiscard]] const std::vector<double> *weights(base::TermId term) const & noexcept;
    const std::vector<double> *weights(base::TermId) const && = delete;
    /** @brief 本词IDF独立值，未知词nullopt；不借用，临时对象可调用。 */
    [[nodiscard]] std::optional<double> inverse_document_frequency(base::TermId term) const noexcept;
    /** @brief 文档未归一化的L2范数独立值，空篇+0，未知文档nullopt。 */
    [[nodiscard]] std::optional<double> document_norm(base::DocumentId document) const noexcept;
    /**
     * @brief 导出拥有型规范文本，固定字段/次序、LF结尾、locale无关
     * @param[in] limits
     *     正max_bytes，包含所有字节；超限在append前拒绝，不返回部分字符串。
     *
     * @return
     *     拥有string或weights.serialization_limits/serialization_bytes诊断；
     *     词键为小写hex字节，浮点为16位小写hex的binary64位值，整数十进制。
     * @throws
     *     分配/未知异常上传。只导出当前部件，不含manifest、文件IO、读取或发布。
     */
    [[nodiscard]] base::Result<std::string> serialize(const WeightSerializationLimits &limits) const;
private:
    WeightedIndex(InvertedIndex raw, std::vector<double> idfs,
                  std::vector<std::vector<double>> weights, std::vector<double> norms) noexcept;
    InvertedIndex raw_; ///< 唯一拥有原始统计/词键/整数关系，不重建词表或变更ID。
    std::vector<double> idfs_; ///< 按TermId对齐，构建/导出消费，有限>=1。
    std::vector<std::vector<double>> weights_; ///< 每词按原始Posting位置对齐，拥有正有限单位权重。
    std::vector<double> norms_; ///< 按原始文档统计位置对齐，空篇+0，其他篇正有限。
};
} // namespace siftwing::index
