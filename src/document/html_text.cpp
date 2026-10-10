/*
 * PROJECT : SIFTWING
 * FILE    : html_text.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用Lexbor解析HTML片段，在资源检查后生成拥有型静态正文
 * IMPLEMENTATION :
 * -- 解析回调在树处理前限制token，C边界不传播异常或修改全局分配器
 * -- 全树结构检查包含template隐藏内容，文字遍历则跳过非正文子树
 * -- 两次非递归深度遍历避免输入嵌套消耗C++调用栈，输出分隔延后到下一段文字
 */

#include "siftwing/document/html_text.h"
#include "siftwing/base/checked.h"

#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/parser.h>
#include <utf8.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace siftwing::document {
namespace {

using Outcome = base::Result<HtmlText>;

Outcome failure(const char* context, const char* message) {
    return Outcome::failure({message, context});
}

struct DocumentDeleter {
    void operator()(lxb_html_document_t* document) const noexcept {
        static_cast<void>(lxb_html_document_destroy(document));
    }
};
struct ParserDeleter {
    void operator()(lxb_html_parser_t* parser) const noexcept {
        static_cast<void>(lxb_html_parser_destroy(parser));
    }
};

// 回调状态只活到同步解析结束；只有整数/指针操作，不在C调用链中构造诊断或抛异常。
struct ParseBudget {
    const HtmlTextLimits& limits;
    lxb_html_tokenizer_token_f original;
    void* original_context;
    std::uint64_t tokens = 0;
    const char* exceeded = nullptr;
};

lxb_html_token_t* bounded_token(lxb_html_tokenizer_t* tokenizer,
                                lxb_html_token_t* token, void* context) noexcept {
    auto& budget = *static_cast<ParseBudget*>(context);
    if (budget.tokens >= budget.limits.max_tokens) {
        budget.exceeded = "html.tokens";
        tokenizer->status = LXB_STATUS_ERROR;
        return nullptr;
    }
    ++budget.tokens;
    auto* result = budget.original(tokenizer, token, budget.original_context);
    if (result == nullptr) { return nullptr; }
    // 元素栈含合成根；最终DOM另查真实节点深度，覆盖HTML修复/模板容器的差异。
    const auto stack_size = lexbor_array_length(tokenizer->tree->open_elements);
    if (stack_size > 0 && stack_size - 1 > budget.limits.max_depth) {
        budget.exceeded = "html.depth";
        tokenizer->status = LXB_STATUS_ERROR;
        return nullptr;
    }
    return result;
}

lxb_dom_node_t* first_child(lxb_dom_node_t* node) noexcept {
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && node->ns == LXB_NS_HTML &&
        node->local_name == LXB_TAG_TEMPLATE) {
        auto* content = lxb_html_interface_template(node)->content;
        return content == nullptr ? nullptr : lxb_dom_interface_node(content);
    }
    return node->first_child;
}

// 只保存当前祖先路径，宽树不一次性压入所有兄弟；template内容容器作为一个节点计量。
const char* validate_tree(lxb_dom_node_t* root, const HtmlTextLimits& limits) {
    struct Frame { lxb_dom_node_t* next; std::uint64_t depth; };
    std::vector<Frame> ancestors{{first_child(root), 0}};
    std::uint64_t count = 0;
    while (!ancestors.empty()) {
        auto& frame = ancestors.back();
        if (frame.next == nullptr) { ancestors.pop_back(); continue; }
        if (frame.depth >= limits.max_depth) { return "html.depth"; }
        if (count >= limits.max_nodes) { return "html.nodes"; }
        ++count;
        auto* node = frame.next;
        frame.next = node->next;
        const auto depth = frame.depth + 1; // 已证明父深度小于正预算，故不会回绕。
        ancestors.push_back({first_child(node), depth});
    }
    return nullptr;
}

bool white_space(std::uint32_t point) noexcept {
    return (point >= 0x0009U && point <= 0x000dU) || point == 0x0020U || point == 0x0085U ||
           point == 0x00a0U || point == 0x1680U || (point >= 0x2000U && point <= 0x200aU) ||
           point == 0x2028U || point == 0x2029U || point == 0x202fU || point == 0x205fU || point == 0x3000U;
}

bool block(lxb_dom_node_t* node) noexcept {
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->ns != LXB_NS_HTML) { return false; }
    switch (node->local_name) {
    case LXB_TAG_ADDRESS: case LXB_TAG_ARTICLE: case LXB_TAG_ASIDE: case LXB_TAG_BLOCKQUOTE:
    case LXB_TAG_DIV: case LXB_TAG_DL: case LXB_TAG_DT: case LXB_TAG_DD: case LXB_TAG_FIELDSET:
    case LXB_TAG_FIGCAPTION: case LXB_TAG_FIGURE: case LXB_TAG_FOOTER: case LXB_TAG_FORM:
    case LXB_TAG_H1: case LXB_TAG_H2: case LXB_TAG_H3: case LXB_TAG_H4: case LXB_TAG_H5:
    case LXB_TAG_H6: case LXB_TAG_HEADER: case LXB_TAG_LI: case LXB_TAG_MAIN: case LXB_TAG_NAV:
    case LXB_TAG_OL: case LXB_TAG_P: case LXB_TAG_PRE: case LXB_TAG_SECTION: case LXB_TAG_TABLE:
    case LXB_TAG_TR: case LXB_TAG_UL: return true;
    default: return false;
    }
}

bool excluded(lxb_dom_node_t* node) noexcept {
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) { return false; }
    // hidden为布尔属性；属性值即使是false也表示存在，不解析style或aria策略。
    const auto* hidden = reinterpret_cast<const lxb_char_t*>("hidden");
    if (lxb_dom_element_has_attribute(lxb_dom_interface_element(node), hidden, 6)) { return true; }
    switch (node->local_name) {
    case LXB_TAG_SCRIPT: case LXB_TAG_STYLE: case LXB_TAG_TEMPLATE: case LXB_TAG_NOSCRIPT:
    case LXB_TAG_HEAD: case LXB_TAG_TITLE: case LXB_TAG_IFRAME: case LXB_TAG_OBJECT:
    case LXB_TAG_EMBED: return true;
    default: return false;
    }
}

bool tag(lxb_dom_node_t* node, lxb_tag_id_t id) noexcept {
    return node->type == LXB_DOM_NODE_TYPE_ELEMENT && node->ns == LXB_NS_HTML && node->local_name == id;
}

class TextBuilder {
public:
    explicit TextBuilder(std::uint64_t limit) : limit_(limit) {}

    void separate(char separator) noexcept {
        if (separator == '\n' || pending_ == 0) { pending_ = separator; }
    }

    bool append(std::string_view text, bool pre) {
        auto position = text.begin();
        while (position != text.end()) {
            const auto* begin = position;
            const auto point = utf8::next(position, text.end());
            if (!pre && white_space(point)) { separate(' '); continue; }
            const std::string_view scalar{begin, static_cast<std::size_t>(position - begin)};
            nonblank_ = nonblank_ || !white_space(point);
            if (!exceeded_ && pending_ != 0 && !text_.empty()) {
                // 分隔已存在时不重复写入；pre内部真实空白不在此折叠。
                if (text_.back() != '\n' && !(pending_ == ' ' && white_space(point) && pre)) {
                    if (!grow(std::string_view{&pending_, 1})) { exceeded_ = true; }
                }
            }
            pending_ = 0;
            if (!exceeded_ && !grow(scalar)) { exceeded_ = true; }
            // pre全空白最终没有正文：超预算后只继续判空，不继续分配；一旦发现文字就拒绝。
            if (exceeded_ && nonblank_) { return false; }
        }
        return true;
    }

    std::string finish() {
        if (!nonblank_) { text_.clear(); }
        return std::move(text_);
    }

private:
    bool grow(std::string_view bytes) {
        auto size = base::checked_narrow<std::uint64_t>(text_.size(), "html.output_bytes");
        if (!size || size.value() > limit_ || bytes.size() > limit_ - size.value() ||
            bytes.size() > text_.max_size() - text_.size()) { return false; }
        text_.append(bytes.data(), bytes.size());
        return true;
    }

    std::uint64_t limit_;
    std::string text_;
    char pending_ = 0; // 分隔只在后续实际文字到来时提交，避免首尾LF及预算误算。
    bool nonblank_ = false;
    bool exceeded_ = false; // 前置pre空白暂超预算时，仍可用有界存储确认最终no_text。
};

bool extract_tree(lxb_dom_node_t* root, TextBuilder& builder) {
    struct Frame { lxb_dom_node_t* node; lxb_dom_node_t* next; bool entered; bool pre; };
    std::vector<Frame> ancestors{{root, nullptr, false, false}};
    while (!ancestors.empty()) {
        auto& frame = ancestors.back();
        auto* node = frame.node;
        if (!frame.entered) {
            if (excluded(node)) { builder.separate(' '); ancestors.pop_back(); continue; }
            if (block(node) || tag(node, LXB_TAG_BR) || tag(node, LXB_TAG_HR)) { builder.separate('\n'); }
            if (tag(node, LXB_TAG_IMG) || tag(node, LXB_TAG_TD) || tag(node, LXB_TAG_TH)) { builder.separate(' '); }
            frame.pre = frame.pre || tag(node, LXB_TAG_PRE);
            if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
                const auto& data = lxb_dom_interface_character_data(node)->data;
                const std::string_view text{reinterpret_cast<const char*>(data.data), data.length};
                if (!builder.append(text, frame.pre)) { return false; }
            }
            frame.next = first_child(node);
            frame.entered = true;
        }
        if (frame.next != nullptr) {
            auto* child = frame.next;
            frame.next = child->next;
            const bool pre = frame.pre;
            ancestors.push_back({child, nullptr, false, pre});
            continue;
        }
        if (block(node)) { builder.separate('\n'); }
        if (tag(node, LXB_TAG_TD) || tag(node, LXB_TAG_TH)) { builder.separate(' '); }
        ancestors.pop_back();
    }
    return true;
}

} // namespace

base::Result<HtmlText> extract_html_text(std::string_view fragment, const HtmlTextLimits& limits) {
    if (limits.max_input_bytes == 0 || limits.max_output_bytes == 0 || limits.max_tokens == 0 ||
        limits.max_nodes == 0 || limits.max_depth == 0) {
        return failure("html.limits", "all HTML extraction limits must be positive");
    }
    auto input_size = base::checked_narrow<std::uint64_t>(fragment.size(), "html.input_bytes");
    if (!input_size || input_size.value() > limits.max_input_bytes) {
        return failure("html.input_bytes", "HTML fragment exceeds input byte limit");
    }
    if (utf8::find_invalid(fragment.begin(), fragment.end()) != fragment.end()) {
        return failure("html.utf8", "HTML fragment is not strict UTF-8");
    }
    if (fragment.find('\0') != fragment.npos) { return failure("html.nul", "HTML fragment contains NUL"); }
    if (fragment.size() >= 3 && fragment.substr(0, 3) == "\xef\xbb\xbf") { fragment.remove_prefix(3); }

    std::unique_ptr<lxb_html_document_t, DocumentDeleter> document{lxb_html_document_create()};
    std::unique_ptr<lxb_html_parser_t, ParserDeleter> parser{lxb_html_parser_create()};
    if (!document || !parser) { throw std::bad_alloc{}; }
    const auto init = lxb_html_parser_init(parser.get());
    if (init == LXB_STATUS_ERROR_MEMORY_ALLOCATION) { throw std::bad_alloc{}; }
    if (init != LXB_STATUS_OK) { return failure("html.parse", "HTML parser initialization failed"); }
    auto* tokenizer = lxb_html_parser_tokenizer(parser.get());
    ParseBudget budget{limits, tokenizer->callback_token_done, tokenizer->callback_token_ctx};
    lxb_html_tokenizer_callback_token_done_set(tokenizer, bounded_token, &budget);
    // 空string_view允许data()==nullptr；传给C解析器的零长度指针仍指向有效对象。
    const auto* bytes = reinterpret_cast<const lxb_char_t*>(fragment.empty() ? "" : fragment.data());
    auto* root = lxb_html_parse_fragment_by_tag_id(parser.get(), document.get(), LXB_TAG_DIV,
                                                 LXB_NS_HTML, bytes, fragment.size());
    if (budget.exceeded != nullptr) { return failure(budget.exceeded, "HTML parsing resource limit exceeded"); }
    if (root == nullptr) {
        if (lxb_html_parser_status(parser.get()) == LXB_STATUS_ERROR_MEMORY_ALLOCATION) { throw std::bad_alloc{}; }
        return failure("html.parse", "HTML fragment parsing failed");
    }
    if (const auto* exceeded = validate_tree(root, limits)) {
        return failure(exceeded, "HTML DOM resource limit exceeded");
    }
    TextBuilder builder(limits.max_output_bytes);
    if (!extract_tree(root, builder)) { return failure("html.output_bytes", "HTML text exceeds output byte limit"); }
    const bool recovered = lexbor_array_obj_length(tokenizer->parse_errors) != 0 ||
                           lexbor_array_obj_length(parser->tree->parse_errors) != 0;
    return Outcome::success({builder.finish(), recovered});
}

} // namespace siftwing::document
