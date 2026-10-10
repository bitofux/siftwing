/*
 * PROJECT : SIFTWING
 * FILE    : html_text_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 用独立字面预期证明HTML实体、文字边界、过滤、恢复、拥有性与资源限额
 * TEST CONTRACT :
 * -- 所有检查为运行期计数并返回失败退出码，在Release下仍有效
 * -- 预期正文不由被测解析函数产生；原始非法字节和临界资源数人工构造
 */

#include "siftwing/document/html_text.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace siftwing::document;
const HtmlTextLimits generous{1024 * 1024, 1024 * 1024, 100000, 100000, 256};
std::size_t checks = 0;
std::size_t failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

void text_case(std::string_view html, std::string_view expected, const char* label) {
    auto result = extract_html_text(html, generous);
    check(result && result.value().text == expected, label);
    if (result && result.value().text != expected) {
        std::cerr << "  expected=[" << expected << "] actual=[" << result.value().text << "]\n";
    }
}

void rejection(std::string_view html, HtmlTextLimits limits, const char* context, const char* label) {
    const auto result = extract_html_text(html, limits);
    check(!result && result.error().context == context && !result.error().message.empty(), label);
}

void words_and_boundaries() {
    text_case("<p>A &amp; B</p><p>中文</p>", "A & B\n中文", "paragraphs and UTF-8 have independent literal expectations");
    text_case(" <b>hel</b>lo <em>世</em>界 ", "hello 世界", "inline boundaries do not split words");
    text_case("a<br>b<br><br>c<hr>d", "a\nb\nc\nd", "br and hr collapse to single paragraph boundaries");
    text_case("<div><p>A</p><p>B</p></div><div>C</div>", "A\nB\nC", "nested empty boundary markers do not multiply LF");
    text_case("<ul><li>甲<li>乙</ul><table><tr><td>A<td>B</tr><tr><td>C<td>D</table>",
              "甲\n乙\nA B\nC D", "HTML optional endings and table cell/row separators are deterministic");
    text_case("\tA\r\n \u3000B&nbsp; C\u0085 D", "A B C D", "Unicode White_Space and NBSP collapse outside pre");
    text_case("&lt;p&gt; &amp;amp; &#65; &#x4E2D; &copy;", "<p> &amp; A 中 ©", "entities decode once and decoded tags remain literal text");
    text_case("\xef\xbb\xbf<p>甲</p>", "甲", "one initial BOM is removed");
    text_case("a\xef\xbb\xbf" "b", "a\xef\xbb\xbf" "b", "interior BOM remains content");
    text_case("<pre>\n  x\r\n\ty  </pre><p>z</p>", "  x\n\ty  \nz", "pre preserves whitespace after HTML newline rules");
    text_case("<p>A</p><pre>x\n y</pre><p>B</p>", "A\nx\n y\nB", "pre boundaries integrate with ordinary paragraphs");
    text_case("<textarea>&lt;b&gt;x&lt;/b&gt;</textarea>", "<b>x</b>", "RCDATA preserves decoded markup as literal text");
}

void filtering_and_recovery() {
    text_case("A<script>throw 42</script><style>.x{color:red}</style><template><p>secret</p></template>B", "A B",
              "script style and template contents do not become text");
    text_case("<div hidden=false>secret</div><p>visible</p><noscript>fallback</noscript>", "visible",
              "boolean hidden and noscript policies exclude whole subtrees");
    text_case("<iframe src='https://invalid.example'>secret</iframe><object>secret</object><embed>ok", "ok",
              "embedded resource subtrees do not become text or trigger requests");
    text_case("<a href='https://invalid.example'>文字</a><img src='https://invalid.example/x' alt='图'>后文", "文字 后文",
              "links keep text, attributes and images do not become content");
    text_case("A<!-- comment -->B", "AB", "comments neither contribute text nor split inline text");
    text_case("<p style='display:none' aria-hidden='true'>文字</p>", "文字", "CSS and aria visibility are explicitly outside extraction policy");
    for (const auto& html : std::vector<std::string>{"", " \t\n\u3000", "<p></p>", "<!--x-->",
        "<script>x</script><template><p>x</p></template>", "<pre>  \t</pre>"}) {
        text_case(html, "", "blank and filtered-only fragments have explicit empty text");
    }
    const auto repaired = extract_html_text("<p>A</bogus>B", generous);
    check(repaired && repaired.value().text == "AB" && repaired.value().recovered,
          "malformed end tag produces text with visible recovery marker");
    const auto clean = extract_html_text("<p>A</p>", generous);
    check(clean && !clean.value().recovered, "valid fragment does not invent recovery warning");
    const auto numeric = extract_html_text("&#0;", generous);
    check(numeric && numeric.value().text == "\xef\xbf\xbd" && numeric.value().recovered,
          "invalid numeric entity uses HTML replacement and recovery marker");
    text_case("<!DOCTYPE html SYSTEM 'https://invalid.example/dtd'><p>A</p>", "A", "HTML doctype does not load external declarations");
}

void limits_and_invalid_input() {
    rejection(std::string_view{"a\0b", 3}, generous, "html.nul", "raw NUL rejects before parser substitution");
    for (const auto& invalid : std::vector<std::string>{"\xff", "\xc0\xaf", "\xed\xa0\x80", "\xe2\x82", "\xf4\x90\x80\x80"}) {
        rejection(invalid, generous, "html.utf8", "invalid UTF-8 rejects without replacement");
    }
    for (unsigned field = 0; field < 5; ++field) {
        auto limits = generous;
        switch (field) {
        case 0: limits.max_input_bytes = 0; break;
        case 1: limits.max_output_bytes = 0; break;
        case 2: limits.max_tokens = 0; break;
        case 3: limits.max_nodes = 0; break;
        default: limits.max_depth = 0; break;
        }
        rejection("x", limits, "html.limits", "each zero budget is a configuration error");
    }
    const auto exact = extract_html_text("<p>中</p>", {10, 3, 4, 2, 2});
    check(exact && exact.value().text == "中", "inclusive input/output/token/node/depth bounds are independent");
    rejection("<p>中</p>", {9, 3, 4, 2, 2}, "html.input_bytes", "input one byte over budget rejects");
    rejection("<p>中</p>", {10, 2, 4, 2, 2}, "html.output_bytes", "output budget counts UTF-8 bytes and rejects whole scalar");
    rejection("<p>中</p>", {10, 3, 3, 2, 2}, "html.tokens", "EOF participates in token budget");
    rejection("<p>中</p>", {10, 3, 4, 1, 2}, "html.nodes", "text node participates in DOM budget");
    rejection("<p>中</p>", {10, 3, 4, 2, 1}, "html.depth", "text node participates in final DOM depth");
    rejection("<div><div><div>x</div></div></div>", {128, 128, 128, 128, 2}, "html.depth", "open element depth stops parsing");
    rejection("<template><p>x</p></template>", {128, 128, 128, 3, 10}, "html.nodes", "excluded template contents still consume nodes");
    rejection("<template><p>x</p></template>", {128, 128, 128, 10, 3}, "html.depth", "template container participates in depth");
    const auto template_exact = extract_html_text("<template><p>x</p></template>", {128, 128, 128, 4, 4});
    check(template_exact && template_exact.value().text.empty(), "template structure exact bounds do not leak hidden text");
    rejection("<div hidden><span>x</span></div>", {128, 128, 128, 2, 10}, "html.nodes", "hidden subtree also consumes structural budget");
    const auto output_exact = extract_html_text("<p>a</p><p>b</p>", {64, 3, 64, 64, 8});
    check(output_exact && output_exact.value().text == "a\nb", "only committed separator consumes output budget");
    rejection("<p>a</p><p>b</p>", {64, 2, 64, 64, 8}, "html.output_bytes", "interior separator counts in output budget");
    const auto blank_pre = extract_html_text("<pre>    </pre>", {64, 1, 64, 64, 8});
    check(blank_pre && blank_pre.value().text.empty(), "blank pre is no-text even when intermediate whitespace exceeds output budget");
    rejection("<pre>    x</pre>", {64, 1, 64, 64, 8}, "html.output_bytes", "pre leading whitespace counts when actual text follows");
    rejection("<pre>    </pre><p>x</p>", {64, 1, 64, 64, 8}, "html.output_bytes", "later paragraph makes preserved pre whitespace part of final text budget");
    const auto max = std::numeric_limits<std::uint64_t>::max();
    check(static_cast<bool>(extract_html_text("x", {max, max, max, max, max})), "wide budgets do not narrow or overflow");
}

HtmlText owning_result() {
    std::string html = "<p>纸船 &amp; 河流</p>";
    const auto original = html;
    auto result = extract_html_text(html, generous);
    check(html == original, "extraction does not modify borrowed input");
    if (!result) { throw std::runtime_error(result.error().message); }
    html.assign(html.size(), 'x');
    return std::move(result).value();
}

void ownership_and_repeated_cleanup() {
    const auto result = owning_result();
    check(result.text == "纸船 & 河流", "returned text survives input and parser destruction");
    for (unsigned repeat = 0; repeat < 40; ++repeat) {
        const auto parsed = extract_html_text("<p>A</bogus>B", generous);
        check(parsed && parsed.value().text == "AB" && parsed.value().recovered, "repeat has no hidden parser state");
        rejection("<p>x</p>", {64, 64, 1, 64, 8}, "html.tokens", "parse abort cleanup remains valid repeatedly");
    }
    std::string nested;
    for (unsigned depth = 0; depth < 100; ++depth) { nested += "<div>"; }
    nested += "x";
    for (unsigned depth = 0; depth < 100; ++depth) { nested += "</div>"; }
    auto parsed = extract_html_text(nested, generous);
    check(parsed && parsed.value().text == "x", "nonrecursive extraction handles nested input within budget");
}
} // namespace

int main() {
    try { words_and_boundaries(); filtering_and_recovery(); limits_and_invalid_input(); ownership_and_repeated_cleanup(); }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL: unexpected exception: " << error.what() << '\n'; }
    std::cout << "HTML text checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
