/*
 * PROJECT : SIFTWING
 * FILE    : txt_reader.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 此模块负责：
 * -- 将一份已装载的TXT字节按一文件一篇转换为拥有型文档结果
 * -- 拥有调用方提供的文档身份/可选标题，保持批量与复读确定性
 * -- 定义严格编码、换行、标题回退、空内容及超限拒绝合同
 */

#pragma once

#include "siftwing/document/document_reader.h"

#include <map>
#include <optional>
#include <string>

namespace siftwing::document {

/** @brief 调用方为一个逻辑TXT输入提供的身份与标题；不自动分配身份。 */
struct TxtDocumentMetadata final {
    base::DocumentId doc_id; ///< 显式身份，零合法；批量唯一性仍由read_documents验证。
    std::optional<std::string> title; ///< 拥有显式标题；缺失、空或全空白时回退，不改变原配置。
};

/**
 * @brief 同步TXT字节适配器；每个输入至多交付一条逻辑序号0的结果
 *
 * @note 配置只读，复读相同输入/配置的结果确定；没有隐式身份计数器。
 *     不装载文件、遍历目录、检查文件系统路径或写回原文；输入装载成本属于调用方。
 * @note 不保存input/sink引用；返回记录拥有文本。未知异常及分配异常上传。
 * @note 不主动共享可变状态；调用方仍须保证输入字节/sink的生命周期与并发访问合法。
 */
class TxtDocumentReader final : public DocumentReader {
public:
    /**
     * @brief 按值取得并拥有输入相对标识到身份/标题的映射
     *
     * @param[in] metadata
     *     键须与将读取的ReaderInput::input_path精确匹配；调用方决定身份，不从文件名推导ID。
     * @note 构造不验证或读取文件；未使用配置不会产生文档。缺失请求配置为调用方违约。
     */
    explicit TxtDocumentReader(std::map<std::string, TxtDocumentMetadata> metadata);

    /**
     * @brief 验证TXT并移交完整记录、明确拒绝或无文字结果
     *
     * @param[in] input
     *     借用已装载字节；合法相对输入名及正数限额通常由read_documents预检。
     * @param[in] limits
     *     输入预算含BOM/原换行；单篇预算为最终标题加规范化正文的UTF-8字节数。
     * @param[in,out] sink
     *     同步借用，最多emit一次；false后立即结束，不保留引用。
     * @note 非TXT拒绝；非法UTF-8拒绝并定位首个非法字节，NUL拒绝，编码不猜测、不替换。
     *     只去除开头UTF-8 BOM，将CRLF/CR变LF，正文其余文字保留，不自动按章/标记切篇。
     *     去BOM后为空或全为Unicode15.1 White_Space时no_text。正常正文不裁剪空白。
     *     非空白显式标题原值优先，否则用文件名去最后扩展名并报告title_fallback警告；
     *     点开头且无其他点的文件名保持。标题须UTF-8合法且无NUL，不另做BOM/换行清洗。
     *     在正文分配前核对最终单篇预算；超限拒绝整篇，等于合法，没有部分提取或截断。
     * @throws std::out_of_range
     *     TXT请求的输入名未配置，是调用方违约，不伪装为文件读取或编码失败。
     * @throws std::invalid_argument
     *     直接调用read时提供零限额；正常经read_documents调用时该请求已被预检拒绝。
     * @throws
     *     标准库分配、sink调用等未预期异常上传。
     */
    void read(const ReaderInput& input, const ReadLimits& limits, ReadSink& sink) override;

private:
    const std::map<std::string, TxtDocumentMetadata> metadata_; ///< 拥有只读配置，无运行期编号状态。
};

} // namespace siftwing::document
