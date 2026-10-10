/*
 * PROJECT : SIFTWING
 * FILE    : html_text.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 定义供RSS正文使用的同步HTML片段抽取与有界拥有型文本合同
 */

#pragma once

#include "siftwing/base/result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace siftwing::document {

/** @brief 调用方显式提供的五项正预算；等于上限合法，零表示配置缺失。 */
struct HtmlTextLimits final {
    std::uint64_t max_input_bytes = 0; ///< 原始片段字节，含开头BOM；解析前检查。
    std::uint64_t max_output_bytes = 0; ///< 最终UTF-8正文的字节预算，字符串增长前检查。
    std::uint64_t max_tokens = 0; ///< 解析回调次数，含文本、结束标签及EOF；树处理前检查。
    std::uint64_t max_nodes = 0; ///< 解析后DOM节点，含过滤子树及template内容容器，不含合成根。
    std::uint64_t max_depth = 0; ///< DOM节点最大深度，根的孩子为1；另限制解析中的元素栈。
};

/** @brief 拥有型抽取结果；空text表示没有正文，与解析恢复标记分别判断。 */
struct HtmlText final {
    std::string text; ///< 拥有UTF-8文字，输入和解析树销毁后仍有效；没有URL或图片内容。
    bool recovered = false; ///< Lexbor记录了tokenizer/tree解析诊断，不证明页面全文完整。
};

/**
 * @brief 在固定div上下文中解析HTML片段，抽取静态文字
 *
 * @param[in] fragment
 *     同步借用的严格UTF-8字节；调用结束前存活且不得并发修改。拒绝原始NUL，去一个开头BOM。
 * @param[in] limits
 *     五项正预算；本函数不装载文件、生成身份或使用批量文本预算。
 *
 * @return
 *     success拥有正文和恢复标记，空正文合法。failure不含截短正文；Error.context区分
 *     html.limits/input_bytes/utf8/nul/tokens/nodes/depth/output_bytes/parse。
 *
 * @note HTML实体按HTML规则还原一次；非法数字实体可被解析器替换为U+FFFD并标记恢复。
 *     普通Unicode15.1 White_Space折为一个ASCII空格，块元素及br边界折为一个LF；首尾
 *     待定分隔不输出，inline标签不自行插入空格。pre保留解析后的空白及换行，HTML规则
 *     可能去掉紧随pre开始标签的首个LF；全空白正文返回空text。
 * @note 过滤script/style/template/noscript/head/title、iframe/object/embed及hidden属性子树；
 *     img/hr提供文字分隔，链接只取文字，注释不输出。过滤子树仍参与结构限额。
 *     不解释CSS/aria-hidden，不识别任意网页的文章主体，不执行JS或加载任何外部资源。
 * @note 每次调用使用独立解析对象及RAII，不改全局allocator；调用方仍须保证借用输入合法。
 *     节点预算在解析后检查，token/元素栈预算在解析中检查；这些不是整个进程内存或CPU硬限额。
 *
 * @throws std::bad_alloc
 *     C解析器报告内存分配失败或C++分配失败；不伪装为可用的正文。
 * @throws
 *     其他未预期标准库异常上传。
 */
[[nodiscard]] base::Result<HtmlText> extract_html_text(std::string_view fragment,
                                                    const HtmlTextLimits& limits);

} // namespace siftwing::document
