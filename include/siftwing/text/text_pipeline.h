/*
 * PROJECT : SIFTWING
 * FILE    : text_pipeline.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 定义建库与查询共享的拥有型文本处理管线
 */
#pragma once

#include "siftwing/text/normalize.h"
#include "siftwing/text/term_frequency.h"

#include <memory>

namespace siftwing::text {

/**
 * @brief 每次调用的阶段预算；全部13项须为正数，等于上限合法
 * @note normalization计原文/规范化字节，tokenization计规范化输入及未过滤token，
 *     analysis计未过滤输入/保留输出及唯一词键。三阶段分别计量，不要求预算大小相等；
 *     建库与查询可以使用不同上限，预算充分时不改变词项语义，不是进程硬资源限。
 */
struct TextPipelineLimits final {
    NormalizationLimits normalization; ///< 拥有原文字节/规范化输出的两正预算值。
    TokenizationLimits tokenization; ///< 拥有规范化输入/未过滤分词输出的四正预算值。
    TokenAnalysisLimits analysis; ///< 拥有未过滤输入/保留序列/TF词键的七正预算值。
};

/**
 * @brief 固定拥有一个分词器和停用集合，供文档正文与查询文字共用同一处理顺序
 * @note 对象不可复制/移动，用unique_ptr转移拥有权；不提供运行时替换资源。
 *     不读文件、不抽取XML/HTML、不累计推荐统计、不编号/去重/生成索引或查询错误。
 *     只处理显式input，调用方决定是否选取正文、标题或查询；原DocumentRecord不修改。
 */
class TextPipeline final {
  public:
    /**
     * @brief 接管已初始化的分词器和已解析的拥有型停用集合
     *
     * @param[in] tokenizer
     *     按值接管独占拥有权；空指针返回pipeline.tokenizer诊断。
     * @param[in] stopwords
     *     按值保存集合；源的复制/移动发生于函数入口，失败不返还已移动资源。
     *
     * @return
     *     成功拥有非空管线，失败仅诊断，不发布半初始化对象。
     *
     * @note 工厂不初始化词典文件，不验证Tokenizer实现与停用规则的资源兼容性。
     * @throws
     *     分配及未知异常向上传递；不保证调用者已移动参数保持原值。
     */
    [[nodiscard]] static base::Result<std::unique_ptr<TextPipeline>>
    create(std::unique_ptr<Tokenizer> tokenizer, StopWords stopwords);

    /** @brief 通过成员RAII释放停用集合和分词器，多态析构不抛异常。 */
    ~TextPipeline() noexcept = default;
    TextPipeline(const TextPipeline &) = delete;
    TextPipeline &operator=(const TextPipeline &) = delete;
    TextPipeline(TextPipeline &&) = delete;
    TextPipeline &operator=(TextPipeline &&) = delete;

    /**
     * @brief 预检全部正配置，再按规范化→分词→停用过滤/单文档TF处理文字
     *
     * @param[in] input
     *     同步借用原UTF-8字节，调用期间存活且无并发写；不剥文件BOM。
     * @param[in] limits
     *     同步借用阶段预算；先完整正配置检查，再按阶段依次检查实际用量。
     *
     * @return
     *     成功拥有TokenAnalysis，保留token顺序/重复及每词实际次数；不借用输入、
     *     临时规范化文字或管线。空文字/无词/全过滤成功空结果；failure无部分载荷。
     *     零预算返回pipeline.limits；阶段失败保留原Error.message/context，不增加包装。
     *
     * @note normalize只作用于分词输入，不对派生Tokenizer输出二次规范化；派生实现须遵守
     *     Tokenizer合同，产生合法词键由analyze_tokens再校验。失败立即返回、不调用后续阶段。
     *     同文本、同规范化规则/分词资源/停用集合及充分预算才保证建库与查询一致；对象身份
     *     不替代未来快照的资源兼容信息。线程安全取决于所拥有Tokenizer的实际const调用合同，
     *     不由const或unique_ptr自动保证；调用方不得通过遗留指针并发修改其资源。
     *
     * @throws
     *     分配及Tokenizer等未知异常原样上传；局部中间结果RAII释放。管线不缓存结果，
     *     不承诺任意派生Tokenizer发生异常后其内部状态的强保证。
     */
    [[nodiscard]] base::Result<TokenAnalysis> analyze(std::string_view input,
                                                     const TextPipelineLimits &limits) const;

  private:
    TextPipeline(std::unique_ptr<Tokenizer> tokenizer, StopWords stopwords);
    std::unique_ptr<Tokenizer> tokenizer_; ///< 拥有已初始化分词器，仅经const tokenize使用。
    StopWords stopwords_; ///< 拥有精确匹配的只读配置，不保存外部字节视图。
};

} // namespace siftwing::text
