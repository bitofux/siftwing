/*
 * PROJECT : SIFTWING
 * FILE    : rss_reader.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 定义RSS2字节到拥有型文档的字段、身份、资源和失败合同
 */

#pragma once

#include "siftwing/document/document_reader.h"
#include "siftwing/document/html_text.h"

#include <map>

namespace siftwing::document {

/** @brief 调用方显式提供的正预算；DOM预算解析后检查，不是整个进程资源硬上限。 */
struct RssParseLimits final {
    std::uint64_t max_xml_nodes = 0; ///< 包括文本/注释/声明，排除document合成节点。
    std::uint64_t max_xml_depth = 0; ///< document孩子深度1；tinyxml2另有500层内部上限。
    HtmlTextLimits html; ///< 每个候选正文字段的HTML预算，输出另取单篇预算的较小值。
};

/**
 * @brief 同步读取无命名空间RSS2/channel/item，按原item位置交付拥有型结果
 *
 * @note 不装载文件、联网、解析外部实体或写回原输入；不支持Atom/RDF及任意转换XML。
 *     XML只接纳UTF-8、XML1.0字符/实体，拒绝DTD及其他<!...>声明。XML坏格式在发布任何
 *     item前失败；DOM结构预算也在逐条抽取前检查。元素/属性前缀须有合法命名空间绑定。
 * @note 正文依次选择content命名空间URI http://purl.org/rss/1.0/modules/content/的encoded、
 *     无命名空间本地content、description。缺失或抽取后无文字才回退；非首选非空结果报告
 *     missing_metadata警告和选中字段。首选失败或超限拒绝该item，不用后续摘要掩盖失败。
 *     字段连接全部直接Text/CDATA，忽略注释/PI；消费的字段有元素孩子时不静默漏内容。
 * @note title是XML解码后的纯文本，只裁剪Unicode15.1首尾空白，不做HTML解析；缺失/空白
 *     用文件名去最后扩展名加#和0基ordinal，并报告title_fallback。重复已识别字段拒绝该item。
 *     link、pubDate和优先dc:creator（同名URI http://purl.org/dc/elements/1.1/）否则author
 *     单独保存裁剪后的原始文本，日期不解析。可选字段缺失为nullopt，提供空字段仍为空字符串。
 *     其他字段忽略，不拼入正文；HTML恢复报告malformed_input警告，不伪称解析完整性。
 * @note 编码/输入字节/字段字节/单篇输出分别使用既有诊断；不支持的XML/HTML结构预算用
 *     rejected/unsupported_format并定位具体预算。坏XML为failed/malformed_input，空输入、
 *     无item或item抽取空正文为no_text。失败/拒绝无部分载荷，普通失败后的继续由共用层决定。
 * @note 配置只读，每次DOM独立；返回文本在DOM/输入销毁后有效。只同步借用input/sink，
 *     sink false后不再抽取或提交下一item；不保证外部输入/sink任意并发安全。
 */
class RssDocumentReader final : public DocumentReader {
public:
    /**
     * @brief 拥有每输入的显式身份向量和解析预算，不隐式分配生产身份
     *
     * @param[in] identities
     *     精确输入名到原item顺序ID向量；包括以后无正文/拒绝的位置，不按成功条目重排。
     *     已成功解析RSS的item数量须与向量长度一致，批量已接纳ID唯一性由共用收集器检查。
     * @param[in] limits
     *     XML两项与HTML五项均为正数；XML节点/深度预算在解析后、抽取前执行。
     *
     * @throws std::invalid_argument
     *     任一解析预算为零。
     */
    explicit RssDocumentReader(std::map<std::string, std::vector<base::DocumentId>> identities,
                               RssParseLimits limits);

    /**
     * @brief 解析一个已装载RSS输入，交付原0基ordinal、拥有型元数据及正文
     *
     * @param[in] input
     *     借用字节和逻辑相对名字；只有rss种类接纳，输入上限含BOM。
     * @param[in] limits
     *     四项正接纳预算；单篇只计最终标题加正文，元数据/DOM仍受输入与解析预算约束。
     * @param[in,out] sink
     *     同步接收端；false后立即结束。无item/文件级失败来源序号为0。
     *
     * @throws std::out_of_range
     *     RSS输入名没有身份配置。
     * @throws std::invalid_argument
     *     直接调用提供零接纳预算，或成功解析RSS的item数与身份数不一致；数量检查在emit前。
     * @throws
     *     分配异常、sink及其他未预期异常上传，不伪装成完整报告。
     */
    void read(const ReaderInput& input, const ReadLimits& limits, ReadSink& sink) override;

private:
    const std::map<std::string, std::vector<base::DocumentId>> identities_; ///< 拥有配置，无运行期编号计数器。
    const RssParseLimits limits_; ///< 拥有不可变资源策略，每次调用使用独立解析状态。
};

} // namespace siftwing::document
