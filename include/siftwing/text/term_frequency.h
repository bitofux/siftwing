/*
 * PROJECT : SIFTWING
 * FILE    : term_frequency.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 定义精确停用词过滤、单文档TF与推荐累计的独立统计合同
 */
#pragma once
#include "siftwing/text/tokenizer.h"
#include <functional>
#include <map>
#include <set>
#include <utility>
namespace siftwing::text {
/** @brief 停用词配置的四正预算；字节为UTF-8，等于上限合法。 */
struct StopWordLimits final {
    std::uint64_t max_input_bytes = 0; ///< 所有源原字节总和，BOM/空行/重复/CRLF均计入。
    std::uint64_t max_entries = 0;     ///< 规范化后唯一非空词条数量。
    std::uint64_t max_entry_bytes = 0; ///< 任一规范化非空词条字节，重复也检查。
    std::uint64_t max_vocabulary_bytes = 0; ///< 唯一词条字节总和，不含容器开销。
};
/**
 * @brief 只读拥有型停用集合；无文件IO、隐式分词、动态增删词或共享全局状态。
 * @note 可按值复制/移动与赋值；moved-from集合有效但内容未指定，读取期不得并发赋值。
 */
class StopWords final {
  public:
    /**
     * @brief 合并显式字节源，按行规范化并一次性发布拥有集合
     *
     * @param[in] sources
     *     调用期间借用全部字节源；配置/总字节后先完整UTF-8/NUL，再解析所有行。
     *     每源仅剥离首个UTF-8 BOM；支持LF/CRLF，行使用normalize_utf8的ASCII小写、
     *     Unicode White_Space折叠/trim。空白行忽略、重复折叠；内部空格拒绝。
     *     #和标点为字面词条，不当注释，不重新分词；中途U+FEFF按字面保留。
     * @param[in] limits
     *     四正预算；空sources/空文件成功为空集合，输入本身不被修改。
     *
     * @return
     *     成功拥有全部词；failure无部分集合。context stopwords.limits/input_bytes/
     *     utf8/nul/entry/entry_bytes/entries/vocabulary_bytes。诊断编码位置含源0基序号/字节。
     *
     * @throws 分配或未知异常向上传递，不转换为普通配置诊断。
     */
    [[nodiscard]] static base::Result<StopWords> parse(const std::vector<std::string_view> &sources,
                                                       const StopWordLimits &limits);
    /** @brief 精确字节查找，不规范化token；调用方须先显式规范化/分词，查询期禁止外部并发写。 */
    [[nodiscard]] bool contains(std::string_view token) const;
    /** @brief 借用只读唯一词集合，按UTF-8字节字典序排列；引用不超过当前对象存活期或赋值。 */
    [[nodiscard]] const std::set<std::string, std::less<>> &entries() const noexcept {
        return entries_;
    }

  private:
    explicit StopWords(std::set<std::string, std::less<>> entries) : entries_(std::move(entries)) {}
    std::set<std::string, std::less<>> entries_;
};
/** @brief 拥有词键的确定性UTF-8字节字典序词频表；每项为正uint64出现次数，无词项ID。 */
using FrequencyTable = std::map<std::string, std::uint64_t, std::less<>>;
/** @brief 单一文档过滤后TF；外部可构造，交给累计前会校验正频及总数一致。 */
struct DocumentTermFrequencies final {
    FrequencyTable terms; ///< 每词在该文档的实际出现次数，不是布尔含词或DF。
    std::uint64_t total_tokens = 0; ///< 必须等于terms所有次数之和；空表仅允许零。
};
/** @brief 一次分析的拥有序列与TF；序列保留顺序、重复及原token大小写。 */
struct TokenAnalysis final {
    TokenSequence tokens; ///< 停用词过滤后的完整序列，不借用输入、StopWords或其TF键。
    DocumentTermFrequencies tf; ///< 与tokens精确一致的单文档统计。
};
/** @brief 一次token分析的七正预算，等于上限合法；不等于进程内存/CPU硬限。 */
struct TokenAnalysisLimits final {
    std::uint64_t max_input_tokens = 0;     ///< 所有输入条数，包括会被滤掉的词。
    std::uint64_t max_input_bytes = 0;      ///< 所有输入token原字节之和。
    std::uint64_t max_token_bytes = 0;      ///< 每个输入token字节，停用词也检查。
    std::uint64_t max_output_tokens = 0;    ///< 保留序列的条数，重复逐次计入。
    std::uint64_t max_output_bytes = 0;     ///< 保留序列字节和，重复逐次计入。
    std::uint64_t max_distinct_terms = 0;   ///< 文档TF唯一词数。
    std::uint64_t max_vocabulary_bytes = 0; ///< 文档TF唯一词键字节和。
};
/**
 * @brief 严格校验全部token后精确过滤，返回拥有序列和单文档TF，不隐式normalize
 *
 * @param[in] input
 *     调用期间同步借用完整token序列，无并发写；所有token须UTF-8、无NUL、非空、
 *     无Unicode15.1 White_Space（范围同normalize）；可含标点/其他合法标量。
 * @param[in] stopwords
 *     只读借用拥有配置，与上游规范化/分词配置兼容由调用方负责，按完整字节匹配。
 * @param[in] limits
 *     先配置/输入条数/总字节，再完整UTF8/NUL，再所有token格式/单token预算，
 *     最后检查保留序列/TF预算；坏的停用token也不能绕过验证。
 *
 * @return
 *     空输入或全过滤成功空序列/空TF；failure仅诊断，context frequency.limits/
 *     input_tokens/input_bytes/utf8/nul/token/token_bytes/output_tokens/output_bytes/
 *     distinct_terms/vocabulary_bytes/overflow。输入不变，无截词或部分序列/统计。
 *
 * @throws 分配或未知异常向上传递。预算不含容器、对象或分配器开销。
 */
[[nodiscard]] base::Result<TokenAnalysis> analyze_tokens(const TokenSequence &input,
                                                         const StopWords &stopwords,
                                                         const TokenAnalysisLimits &limits);
/** @brief 推荐语料累计表，与单文档TF/搜索DF分开；输入集合选择由调用方承担。 */
struct RecommendationFrequencies final {
    FrequencyTable terms; ///< 所有显式交付文档TF的出现次数之和，不默认搜索去重。
    std::uint64_t total_tokens = 0; ///< 必须等于terms次数之和；uint64溢出拒绝。
    std::uint64_t documents = 0; ///< 已交付次数，空文档也计1；无DocId/隐式幂等或去重。
};
/** @brief 每次累计后的五正预算；等于上限合法，配置不可用零表示无限。 */
struct RecommendationLimits final {
    std::uint64_t max_documents = 0; ///< 接受的文档交付次数，非去重文档数或搜索N。
    std::uint64_t max_distinct_terms = 0;   ///< 累计唯一词数。
    std::uint64_t max_term_bytes = 0;       ///< 任何已有/新词键字节。
    std::uint64_t max_vocabulary_bytes = 0; ///< 累计唯一词键字节和。
    std::uint64_t max_total_tokens = 0;     ///< 累计次数总和，非输入序列装载预算。
};
/**
 * @brief 将一个文档TF加入推荐统计并返回新表，失败/异常不修改已有统计
 *
 * @param[in] current
 *     同步借用此前累计；键UTF8、无NUL/空白/空词、正次数，总和须精确匹配total_tokens。
 *     documents为零时必须为空/总数零；documents为正允许空表（此前全过滤/空文档）。
 * @param[in] document
 *     借用一个完整文档TF；同样校验键/正次数/总和及预算，不再过滤/规范化。
 *     重复交付计重复频次；输入集合、过滤配置及资源兼容由调用方明确选择。
 * @param[in] limits
 *     先正配置，再current/document合法性及现有预算，再受检加文档数/总次数和合并词表。
 *
 * @return
 *     成功新拥有表；失败仅诊断，无部分累计。context recommendation.limits/table/utf8/
 *     nul/term/term_bytes/distinct_terms/vocabulary_bytes/documents/total_tokens/overflow。
 *     不计算搜索DF/N/TF-IDF，不存储文档身份，不提供去重或持久化。
 *
 * @throws 分配或未知异常向上传递；拷贝已有词表的成本不受进程硬内存预算保证。
 */
[[nodiscard]] base::Result<RecommendationFrequencies>
accumulate_recommendation(const RecommendationFrequencies &current,
                          const DocumentTermFrequencies &document,
                          const RecommendationLimits &limits);
} // namespace siftwing::text
