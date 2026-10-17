/*
 * PROJECT : SIFTWING
 * FILE    : query.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-12
 * BRIEF   : 查询文本、匹配模式与结果条数的拥有型业务合同
 */
#pragma once

#include "siftwing/text/text_pipeline.hpp"

namespace siftwing::query {

/** @brief all要求每个有效词命中，any要求至少一个命中；不是词序或短语匹配。 */
enum class QueryMode {
    all,
    any
};

/** @brief 查询选项；默认OR/10条，不解释输入文字中的AND、OR或其他运算符。 */
struct QueryOptions final {
    QueryMode mode = QueryMode::any; ///< all为AND，any为OR；其他枚举值拒绝。
    std::uint64_t top_k = 10; ///< 请求结果上限，须在[1,QueryLimits::max_top_k]内，不保证命中数量。
};

/** @brief 显式正预算；文本三阶段沿已有合同，条数上限含端点，不是进程硬资源限。 */
struct QueryLimits final {
    text::TextPipelineLimits text; ///< 原查询、规范化、未过滤/过滤token与唯一词键各自计量。
    std::uint64_t max_top_k = 0; ///< 正uint64上限；0不表示无限，默认选项也不自动缩小。
};

/**
 * @brief 拥有规范化/分词/停用过滤后的查询TF与选项，无原文、词ID、快照或管线借用
 * @note 工厂只发布非空有效查询；可独立复制，不可赋值。noexcept移动完整转交，源TF清空
 *     且总次数归零、选项保持；移空源仅供观察/再次移动或析构，不作为有效业务查询。
 *     getter借用在所有者移动/析构后失效，不能与这些动作并发；独立只读对象无可变缓存。
 */
class Query final {
public:
    /** @brief 独立复制完整查询，分配异常上传；原对象不变。 */
    Query(const Query &) = default;
    /** @brief 转交拥有TF并显式清空源，所有原借用失效。 */
    Query(Query &&other) noexcept;
    Query &operator=(const Query &) = delete; ///< 不在已发布查询上替换状态。
    Query &operator=(Query &&) = delete; ///< 使用构造转交，保持已发布查询只读。
    /** @brief 释放拥有词键，无管线资源或文件句柄。 */
    ~Query() noexcept = default;

    /**
     * @brief 验证选项，再复用共享文本管线构造拥有型查询
     *
     * @param[in] input
     *     同步借用原UTF-8，调用期间稳定；词边界/大小写/停用规则由pipeline决定。
     * @param[in] pipeline
     *     同步借用已初始化管线；调用方保证建库与查询的规则/资源兼容。
     * @param[in] options
     *     按值取得模式和请求条数，不从词项数量推导、不截断或修正非法值。
     * @param[in] limits
     *     max_top_k须为正；随后检查mode、top_k，再由pipeline检查13项正文本预算和文字。
     *
     * @return
     *     非空查询或query.limits/mode/top_k/empty诊断；文本阶段错误原样保留。
     *     空原文、无词或全停用均为query.empty；未知词仍为有效词，是否命中由后续匹配决定。
     *     TF按UTF-8字节序存储正次数，总数含重复；不保存词序，不分配TermId或访问索引。
     *
     * @note all/any针对不同的有效词键；重复词不增加AND约束，但次数供后续查询向量使用。
     *     后续AND遇未知词应为空结果，OR忽略未知词；全部未知仍为空结果，不返回query.empty。
     *     本模块不生成候选、打分、排序、推荐或持久化，不验证实际快照资源声明。
     * @throws 分配与管线未知异常上传，局部对象RAII释放，failure无部分查询；不修改input。
     */
    [[nodiscard]] static base::Result<Query> from_text(std::string_view input,
        const text::TextPipeline &pipeline,
        QueryOptions options,
        const QueryLimits &limits);

    /** @brief 借用拥有TF，不能越过当前所有者生命；临时对象禁止借用，移空源为完整空TF。 */
    [[nodiscard]] const text::DocumentTermFrequencies &frequencies() const & noexcept;
    const text::DocumentTermFrequencies &frequencies() const && = delete;
    /** @brief 按值返回AND/OR选择，移动后源选项保持。 */
    [[nodiscard]] QueryMode mode() const noexcept {
        return options_.mode;
    }
    /** @brief 按值返回请求条数，不表示已有结果数量。 */
    [[nodiscard]] std::uint64_t top_k() const noexcept {
        return options_.top_k;
    }
    /** @brief 识别移空对象；工厂发布的成功对象必为false。 */
    [[nodiscard]] bool empty() const noexcept {
        return frequencies_.terms.empty();
    }

private:
    Query(text::DocumentTermFrequencies frequencies, QueryOptions options);
    text::DocumentTermFrequencies frequencies_; ///< 拥有唯一词键及重复次数；总次数严格等于次数和。
    QueryOptions options_; ///< 拥有已校验选项，复制/移动不重新解析。
};

} // namespace siftwing::query
