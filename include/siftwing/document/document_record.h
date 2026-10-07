/*
 * PROJECT : SIFTWING
 * FILE    : document_record.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 此模块负责：
 * -- 用拥有型值记录统一抽取文档的身份、标题、正文及来源元数据
 * -- 区分内部文档身份与文件内逻辑记录的来源定位
 * -- 不负责读取、校验、编号生成、文本分析或索引存储格式
 */

#pragma once

#include "siftwing/base/id.h"

#include <cstdint>
#include <optional>
#include <string>

namespace siftwing::document {

/**
 * @brief 当前输入合同的来源种类；枚举数值不构成磁盘或网络编码
 *
 * @note 种类只描述生产者提供的来源语义，不证明已经执行对应格式的解析。
 */
enum class SourceKind {
    /** 通用或样本驱动的 TXT 输入，切篇策略由读取器决定。 */
    txt,
    /** RSS XML 输入，逻辑记录以 item 定位。 */
    rss,
    /** 内部结构化 JSONL 输入，逻辑记录规则由读取器决定。 */
    jsonl
};

/**
 * @brief 用输入标识和文件内逻辑序号回溯一篇来源记录
 *
 * @note 两字段共同定位记录；相同输入在不同输入根或不同内容版本中不保证身份相同。
 * @warning 本类型仅保存值，不访问文件或验证路径；生产者负责非空、相对输入标识及定位规则。
 */
struct SourceIdentity final {
    /** 拥有输入根下的来源相对标识；不是输出路径，不自动规范化或检查路径安全。 */
    std::string input_path;

    /** 文件内从 0 开始的逻辑记录序号；单篇 TXT 为 0，不是文档 ID、行号或字节偏移。 */
    std::uint64_t record_ordinal = 0;
};

/**
 * @brief 独立于标题/正文保存的拥有型来源元数据
 *
 * @note 可选字符串的 nullopt 表示未提供，已提供的空字符串仍为空；不自动互相转换。
 * @note 默认值是便于组装的空 TXT 来源，不证明生产输入有效或可追溯。
 */
struct DocumentSource final {
    /** 输入种类；调用者应按实际生产者填写，不从扩展名或正文自动推断。 */
    SourceKind kind = SourceKind::txt;

    /** 拥有文件及逻辑记录的来源定位；URL、日期与标题不代替此定位。 */
    SourceIdentity source_id;

    /** 拥有来源 URL 原值，可缺失或为空；不校验语法，不联网，也不作为唯一身份。 */
    std::optional<std::string> url;

    /** 拥有原始作者文本，可缺失或为空；不追加到索引正文。 */
    std::optional<std::string> author;

    /** 拥有原始发布日期文本，可缺失或为空；不表示已解析的时间或文档更新时间。 */
    std::optional<std::string> published_at_raw;
};

/**
 * @brief 一篇统一抽取文档的拥有型、可修改值记录
 *
 * 成员为公开值，支持聚合初始化、复制/移动构造及赋值。内部身份必须由调用者显式提供，
 * 无默认身份或编号分配；来源定位与内部身份是不同含义，不保证编号唯一、连续或等于下标。
 *
 * @note 标题和正文的生产合同为 UTF-8 抽取文本；此容器不验证编码、空白、容量或抽取完整性。
 *     空标题/正文可表示，构造记录不等于读取或索引成功。校验、回退和报告由读取器负责。
 * @note 所有字符串及可选元数据均拥有存储，输入缓冲区销毁后不失效。复制产生独立载荷，
 *     移动后源成员遵守各自类型的有效但内容未指定状态，不保证源字符串保留原值。
 * @note 无 IO、fd、视图或共享资源；销毁由成员 RAII 释放。独立对象可独立使用，同对象
 *     并发只读可行，存在写入时由调用者同步；借用成员引用不延长记录生命周期。
 * @throws
 *     字符串或成员构造/复制的分配异常向上传递；没有业务 Result、自动异常捕获或通用
 *     noexcept 构造承诺。
 * @warning 不冻结序列化布局；哈希、SimHash、分词、去重决策和索引权重由各分析阶段处理。
 */
struct DocumentRecord final {
    /** 显式提供的内部文档身份；不会因来源/标题变化而自动重新编号。 */
    base::DocumentId doc_id;

    /** 拥有已抽取标题文本；允许为空，不在容器中实施标题回退或规范化。 */
    std::string title;

    /** 拥有已抽取正文文本；允许为空，不拼接来源元数据或保留分析结果。 */
    std::string content;

    /** 拥有独立来源元数据；不证明输入存在、可追溯或抽取已经成功。 */
    DocumentSource source;
};

}  // namespace siftwing::document
