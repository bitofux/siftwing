/*
 * PROJECT : SIFTWING
 * FILE    : lexicon.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 定义固定词表的局部身份和独立推荐词频/Unicode字符索引
 */
#pragma once
#include "siftwing/base/id.h"
#include "siftwing/text/term_frequency.h"

#include <optional>
#include <vector>

namespace siftwing::index {
/** @brief 编号规则版本，不是快照版本；同词表按UTF-8字节序唯一排序后从0编号。 */
inline constexpr std::uint32_t lexicon_policy_version = 1;

/** @brief 五项正预算，等于上限合法；不含容器/排序开销，不是进程硬资源限制。 */
struct LexiconLimits final {
    std::uint64_t max_input_terms = 0; ///< 输入条数，重复逐次计入。
    std::uint64_t max_input_bytes = 0; ///< 输入词键字节和，重复逐次计入。
    std::uint64_t max_term_bytes = 0; ///< 每个输入词键UTF-8字节数。
    std::uint64_t max_distinct_terms = 0; ///< 唯一词数，亦限制局部ID数量。
    std::uint64_t max_vocabulary_bytes = 0; ///< 唯一词键字节和。
};

/**
 * @brief 只读拥有词表；ID等于唯一排序词的位置，禁止运行时增删
 * @note 同一词集合的输入顺序/重复不影响ID；不同词表可改变ID，不能跨快照或搜索/推荐
 *     词典混用。TermId只检查强类型，不携带词典身份。上游负责规范化/分词资源兼容。
 * @note 不可复制，noexcept移动/赋值，源成为空词表，自移动保持；借用在移动/赋值/析构
 *     后失效。同步只读查询可独立执行，不能与所有者移动/析构并发；未实测并发。
 */
class Lexicon final {
public:
    Lexicon(const Lexicon &) = delete;
    Lexicon &operator=(const Lexicon &) = delete;
    /** @brief 转交拥有词表，源清空，不改变目标的词项身份。 */
    Lexicon(Lexicon &&other) noexcept;
    /** @brief 释放原词表后转交；自移动保持，源清空。 */
    Lexicon &operator=(Lexicon &&other) noexcept;

    /**
     * @brief 完整验证词键后排序去重并复制拥有，不隐式规范化或过滤
     * @param[in] terms
     *     同步借用词键，调用期间稳定。严格UTF-8、无NUL、非空、无Unicode15.1 White_Space；
     *     原大小写/标点/其他合法标量保留，重复合法。空输入成功空词表。
     * @param[in] limits
     *     先正配置/输入条数/字节预算，再所有UTF-8/NUL，再词形，最后唯一输出预算。
     *
     * @return
     *     拥有完整Lexicon或lexicon.limits/input_terms/input_bytes/term_bytes/utf8/nul/
     *     term/distinct_terms/vocabulary_bytes诊断，无部分词表，输入不变。
     * @throws
     *     分配及未知异常上传；排序借用不逃逸。
     */
    [[nodiscard]] static base::Result<Lexicon> build(
        const std::vector<std::string_view> &terms, const LexiconLimits &limits);

    /**
     * @brief 精确字节二分查找，不验证/规范化查询词，也不产生查询错误
     * @param[in] term
     *     调用期间稳定字节，空/非法或未知键仅无匹配。
     *
     * @return
     *     局部强ID值或nullopt，不借用词表。
     */
    [[nodiscard]] std::optional<base::TermId> find(std::string_view term) const;

    /**
     * @brief 按局部ID借用词键，未知ID返回nullopt
     * @param[in] id
     *     只属于本词表的身份，0合法；跨词表误用由调用方负责。
     *
     * @return
     *     只读string_view，有效至所有者移动/赋值/析构，禁止并发修改所有者。
     */
    [[nodiscard]] std::optional<std::string_view> term(base::TermId id) const & noexcept;
    /** @brief 临时词表不能返回即将失效的借用。 */
    std::optional<std::string_view> term(base::TermId) const && = delete;
    /** @brief 借用按ID/字节序排列的拥有词表，失效条件同term，不允许修改。 */
    [[nodiscard]] const std::vector<std::string> &terms() const & noexcept { return terms_; }
    const std::vector<std::string> &terms() const && = delete;

private:
    explicit Lexicon(std::vector<std::string> terms) noexcept : terms_(std::move(terms)) {}
    std::vector<std::string> terms_; ///< 唯一拥有词键；位置即本词表局部TermId。
};

/** @brief 推荐字符索引：Unicode标量按数值排序，每个词内重复字符只贡献一个递增局部ID。 */
using CharacterIndex = std::map<char32_t, std::vector<base::TermId>>;

/** @brief 九项正预算；lexicon计输入/词表，其余计推荐统计与字符关系。 */
struct RecommendationDictionaryLimits final {
    LexiconLimits lexicon; ///< 输入为M1.13唯一频次表，但仍核全套词键预算。
    std::uint64_t max_documents = 0; ///< 显式交付次数，不是搜索去重后N。
    std::uint64_t max_total_tokens = 0; ///< 累计出现次数，不是DF。
    std::uint64_t max_characters = 0; ///< 不同Unicode标量键数量。
    std::uint64_t max_character_links = 0; ///< 唯一(标量,TermId)关系数，不按词频重复。
};

/**
 * @brief 拥有独立推荐Lexicon、累计频次与字符索引，不默认搜索去重
 * @note 输入来自显式选择的M1.13推荐累计；documents=0必须空表/零次数，正交付数允许空表。
 *     词项ID仅属本推荐词典，不能拿搜索词典的同数值ID解释。字符为Unicode标量，非字素；
 *     不NFC、不二次规范化/分词/停用过滤，不计算候选并集、编辑距离、DF/N或TF-IDF。
 * @note 不可复制，noexcept移动/赋值，源清为空词表/频次/字符索引/零统计，自移动保持；
 *     借用均在移动/赋值/析构后失效。无文件IO、共享可变缓存或资源加载。
 */
class RecommendationDictionary final {
public:
    RecommendationDictionary(const RecommendationDictionary &) = delete;
    RecommendationDictionary &operator=(const RecommendationDictionary &) = delete;
    RecommendationDictionary(RecommendationDictionary &&other) noexcept;
    RecommendationDictionary &operator=(RecommendationDictionary &&other) noexcept;

    /**
     * @brief 完整验证累计表，生成确定性的拥有词频词典与字符索引
     * @param[in] frequencies
     *     同步稳定借用；正词频，受检总和等于total_tokens，documents统计同M1.13。
     * @param[in] limits
     *     全部正预算先验，输出字符/关系预算在对应插入前检查；等于上限合法。
     *
     * @return
     *     完整拥有对象或dictionary.limits/documents/table/overflow/total_tokens/
     *     characters/character_links诊断；词键失败沿用lexicon.*，无部分对象，输入不变。
     * @throws
     *     分配及未知异常上传，局部容器RAII释放；预算不含容器或临时内存开销。
     */
    [[nodiscard]] static base::Result<RecommendationDictionary> build(
        const text::RecommendationFrequencies &frequencies,
        const RecommendationDictionaryLimits &limits);

    /** @brief 借用本词典Lexicon，所有者移动/赋值/析构后失效。 */
    [[nodiscard]] const Lexicon &lexicon() const & noexcept { return lexicon_; }
    const Lexicon &lexicon() const && = delete;
    /** @brief 借用按局部ID排列的正频次，与lexicon词表等长；失效条件同lexicon。 */
    [[nodiscard]] const std::vector<std::uint64_t> &frequencies() const & noexcept { return frequencies_; }
    const std::vector<std::uint64_t> &frequencies() const && = delete;
    /** @brief 借用按标量数值排序的字符索引及递增唯一ID列表；失效条件同lexicon。 */
    [[nodiscard]] const CharacterIndex &characters() const & noexcept { return characters_; }
    const CharacterIndex &characters() const && = delete;
    /** @brief 查询本词典局部ID的次数值，未知ID返回nullopt，不产生借用。 */
    [[nodiscard]] std::optional<std::uint64_t> frequency(base::TermId id) const noexcept;
    /** @brief 拥有的原推荐交付次数值；不自动去重，不是搜索文档数N。 */
    [[nodiscard]] std::uint64_t documents() const noexcept { return documents_; }
    /** @brief 拥有的累计次数值，等于全部frequency之和，不是DF。 */
    [[nodiscard]] std::uint64_t total_tokens() const noexcept { return total_tokens_; }

private:
    RecommendationDictionary(Lexicon lexicon, std::vector<std::uint64_t> frequencies,
        CharacterIndex characters, std::uint64_t documents, std::uint64_t total_tokens) noexcept;
    Lexicon lexicon_; ///< 拥有本推荐词表，搜索Lexicon完全独立。
    std::vector<std::uint64_t> frequencies_; ///< 拥有ID→出现次数，与词表位置一致。
    CharacterIndex characters_; ///< 拥有标量→局部ID关系，无文档身份。
    std::uint64_t documents_; ///< 已验证推荐交付次数，移动源归零。
    std::uint64_t total_tokens_; ///< 已验证累计正频总和，移动源归零。
};
} // namespace siftwing::index
