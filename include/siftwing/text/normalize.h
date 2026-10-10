/*
 * PROJECT : SIFTWING
 * FILE    : normalize.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 定义建库与查询可共用的严格UTF-8文本规范化合同
 */

#pragma once

#include "siftwing/base/result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace siftwing::text {

/** @brief 固定规则版本；语义变化须另定版本并评估未来索引/查询兼容，不是快照身份。 */
inline constexpr std::uint32_t normalization_policy_version = 1;

/** @brief 调用方显式提供的正字节上限；输入与最终输出分别计数，等于上限合法。 */
struct NormalizationLimits final {
    std::uint64_t max_input_bytes = 0; ///< 原始输入UTF-8字节数；验证/输出分配前检查。
    std::uint64_t max_output_bytes = 0; ///< 最终规范化字符串字节数；完整预计算后才分配。
};

/**
 * @brief 严格校验UTF-8并按固定版本规则产生拥有型规范化文字
 *
 * @param[in] input
 *     同步借用完整字节，调用期间存活且无并发写入；拒绝非法UTF-8和NUL，不替换非法编码。
 * @param[in] limits
 *     两项正预算。先检查配置/输入大小，再完整编码/NUL，最后检查最终输出大小。
 *
 * @return
 *     success拥有UTF-8 string，空或全空白输入成功返回空串；failure只拥有诊断，无截短载荷。
 *     Error.context为normalize.limits/input_bytes/utf8/nul/output_bytes；非法编码诊断含首个
 *     非法序列开始的0基字节位置，NUL诊断含其0基字节位置。返回值不借用input或limits。
 *
 * @note ASCII A—Z转a—z；Unicode15.1 White_Space折成单ASCII空格并去首尾空白，CR/LF等
 *     也折空格。数字、标点、中文、非ASCII大小写及其他合法码点原样保留。U+FEFF不是该
 *     空白集合，所有位置都保留；文件BOM处理仍属于输入适配器。不按locale分类，不做
 *     完整Unicode case folding、NFC/NFKC、实体/HTML解析、分词或停用词过滤。
 * @note 相同输入和规则输出确定；在两次调用预算均足够时normalize(normalize(x))不变。
 *     原DocumentRecord/显示文本不修改；此函数是后续索引和查询的共享入口，尚不组装管线。
 *     不读取文件或联网，无共享可变状态，独立调用可并发；输入/limits的并发写由调用方同步。
 *     两次码点遍历，按最终大小reserve后填充；字节限额不是整个进程内存/CPU硬上限。
 *
 * @throws
 *     分配失败及其他未预期标准库异常上传，不伪装为成功或部分正文。
 */
[[nodiscard]] base::Result<std::string> normalize_utf8(std::string_view input,
                                                     const NormalizationLimits& limits);

} // namespace siftwing::text
