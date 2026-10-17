#!/usr/bin/env python3
"""原创临时HTML的命令行行为验证；使用独立XML解析器核对RSS，不访问网络。"""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from bs4 import BeautifulSoup


SCRIPT = Path(__file__).resolve().parents[1]/"html_to_rss.py"
CONTENT = "{http://purl.org/rss/1.0/modules/content/}encoded"


class HtmlToRssTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="siftwing-html-rss-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.input = self.root/"html"
        self.input.mkdir()
        self.output = self.root/"rss"

    def html(self, name="example.html", content="<main><h1>标题</h1><p>完整正文</p></main>", encoding="utf-8"):
        path = self.input/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode(encoding))
        return path

    def run_tool(self, *extra, code=0):
        result = subprocess.run(
            [sys.executable, *(["-O"] if sys.flags.optimize else []), "-B", str(SCRIPT), "--input", str(self.input),
             "--output", str(self.output), *extra],
            capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(result.returncode, code, result.stdout+result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        return result

    def report(self):
        return json.loads((self.output/"report.json").read_text(encoding="utf-8"))

    def items(self):
        return [item for path in sorted(self.output.glob("*.xml"))
                for item in ET.parse(path).getroot().findall("channel/item")]

    def test_full_body_code_entities_and_input_unchanged(self):
        # 独立文字预期证明尖括号未被误识为标签、缩进未被strip破坏。
        source = self.html(content='''<html><head><meta charset="utf-8"><title>页面标题</title></head>
<body><nav>不要导航</nav><main><h1>中文标题</h1><p>A &amp; B <em>正文</em></p>
<script>不要脚本</script><style>不要样式</style><aside>不要侧栏</aside>
<pre><code>std::vector&lt;int&gt; x;
    if (a &lt; b &amp;&amp; c &gt; d) {
        x.push_back(1);
    }</code></pre><table><tr><td>表格内容</td></tr></table>
<p>尾部完整正文 ]]&gt; END</p></main><footer>不要页脚</footer></body></html>''')
        before = source.read_bytes()
        self.run_tool()
        self.assertEqual(source.read_bytes(), before)
        item, = self.items()
        self.assertEqual(item.findtext("title"), "中文标题")
        body = BeautifulSoup(item.findtext(CONTENT), "lxml")
        self.assertEqual(body.pre.get_text(), "std::vector<int> x;\n    if (a < b && c > d) {\n        x.push_back(1);\n    }")
        text = body.get_text()
        for excluded in ("不要导航", "不要脚本", "不要样式", "不要侧栏", "不要页脚"):
            self.assertNotIn(excluded, text)
        for expected in ("A & B", "表格内容", "尾部完整正文 ]]> END"):
            self.assertIn(expected, text)
        self.assertEqual(item.findtext("link"), source.as_uri())
        record, = self.report()["documents"]
        self.assertEqual(record["sha256"], hashlib.sha256(before).hexdigest())

    def test_recursive_case_insensitive_suffix_and_url_mapping(self):
        self.html("z.HTM")
        self.html("sub/中文 文件.HTML")
        (self.input/"ignore.txt").write_text("not html")
        self.run_tool("--base-url", "https://example.invalid/docs")
        records = self.report()["documents"]
        self.assertEqual([r["path"] for r in records], ["sub/中文 文件.HTML", "z.HTM"])
        self.assertEqual([item.findtext("link") for item in self.items()], [
            "https://example.invalid/docs/sub/%E4%B8%AD%E6%96%87%20%E6%96%87%E4%BB%B6.HTML",
            "https://example.invalid/docs/z.HTM",
        ])

    def test_canonical_url_and_explicit_base_precedence(self):
        self.html(content='<head><link rel="canonical" href="https://example.invalid/original"></head><main>正文</main>')
        self.run_tool()
        self.assertEqual(self.items()[0].findtext("link"), "https://example.invalid/original")
        self.output = self.root/"mapped"
        self.run_tool("--base-url", "https://example.invalid/mapped/")
        self.assertEqual(self.items()[0].findtext("link"), "https://example.invalid/mapped/example.html")

    def test_og_url_and_relative_canonical_fallback(self):
        self.html(content='<head><link rel="canonical" href="relative.html"><meta property="og:url" content="https://example.invalid/og"></head><main>正文</main>')
        self.run_tool()
        self.assertEqual(self.items()[0].findtext("link"), "https://example.invalid/og")

    def test_explicit_selector_and_extra_exclusion(self):
        self.html(content='<body><p>外部文字</p><div id="doc"><h1>文档</h1><p>正文</p><div class="ad">广告</div></div></body>')
        self.run_tool("--selector", "#doc", "--exclude-selector", ".ad")
        body = self.items()[0].findtext(CONTENT)
        self.assertIn("正文", body)
        self.assertNotIn("外部文字", body)
        self.assertNotIn("广告", body)

    def test_title_fallback_and_body_warning(self):
        self.html("fallback.html", "<body><p>Body text</p></body>")
        self.run_tool()
        item, = self.items()
        self.assertEqual(item.findtext("title"), "fallback")
        self.assertEqual(self.report()["documents"][0]["warnings"],
                         ["title_from_filename", "body_fallback_check_navigation"])

    def test_count_based_batches_keep_all_documents_in_order(self):
        for number in range(5):
            self.html(f"{number}.html", f"<main><h1>{number}</h1><p>body {number}</p></main>")
        self.run_tool("--batch-size", "2")
        self.assertEqual([item.findtext("title") for item in self.items()], list("01234"))
        self.assertEqual([feed["items"] for feed in self.report()["feeds"]], [2, 2, 1])

    def test_actual_xml_byte_budget_splits_and_hashes_match(self):
        for number in range(4):
            self.html(f"{number}.html", f'<main><h1>{number}</h1><p>{"&amp;"*300}</p></main>')
        self.run_tool("--max-feed-bytes", "5500")
        report = self.report()
        self.assertEqual(report["summary"]["converted"], 4)
        self.assertGreater(report["summary"]["rss_files"], 1)
        for feed in report["feeds"]:
            raw = (self.output/feed["path"]).read_bytes()
            self.assertLessEqual(len(raw), 5500)
            self.assertEqual(feed["bytes"], len(raw))
            self.assertEqual(feed["sha256"], hashlib.sha256(raw).hexdigest())

    def test_oversized_single_document_is_failure_without_truncation(self):
        self.html(content="<main>" + "x"*5000 + "</main>")
        self.run_tool("--max-feed-bytes", "1000", code=1)
        self.assertEqual(self.items(), [])
        self.assertIn("未截断", self.report()["documents"][0]["error"])

    def test_partial_failures_are_visible_and_good_files_continue(self):
        self.html("a.html")
        self.html("b.html", "")
        self.html("c.html", "<main><img src='no-fetch.png'></main>")
        self.run_tool(code=1)
        self.assertEqual(self.report()["summary"]["converted"], 1)
        self.assertEqual(self.report()["summary"]["failed"], 2)
        self.assertEqual(len(self.items()), 1)

    def test_explicit_selector_miss_never_silently_falls_back(self):
        self.html()
        self.run_tool("--selector", "#missing", code=1)
        self.assertEqual(self.report()["summary"]["failed"], 1)
        self.assertEqual(self.items(), [])

    def test_multiple_regions_do_not_silently_drop_siblings(self):
        self.html(content="<body><article>第一篇正文</article><article>第二篇正文</article></body>")
        self.run_tool()
        body = self.items()[0].findtext(CONTENT)
        self.assertIn("第一篇正文", body)
        self.assertIn("第二篇正文", body)
        self.output = self.root/"explicit"
        self.run_tool("--selector", "article", code=1)
        self.assertIn("多个区域", self.report()["documents"][0]["error"])

    def test_explicit_encoding_and_html_declared_encoding(self):
        self.html(content='<meta charset="gb18030"><main><h1>中文</h1>中文正文</main>', encoding="gb18030")
        self.run_tool()
        self.assertEqual(self.items()[0].findtext("title"), "中文")
        self.output = self.root/"explicit"
        self.run_tool("--encoding", "gb18030")
        self.assertEqual(self.items()[0].findtext("title"), "中文")
        self.output = self.root/"wrong"
        self.run_tool("--encoding", "utf-8", code=1)

    def test_html_read_limit_is_failure(self):
        self.html()
        self.run_tool("--max-html-bytes", "8", code=1)
        self.assertIn("max-html-bytes", self.report()["documents"][0]["error"])

    def test_existing_output_is_preserved(self):
        self.html()
        self.output.mkdir()
        sentinel = self.output/"keep.xml"
        sentinel.write_bytes(b"do not overwrite")
        self.run_tool(code=2)
        self.assertEqual(list(self.output.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b"do not overwrite")

    def test_invalid_arguments_produce_no_output(self):
        self.html()
        for args in (("--selector", "["), ("--exclude-selector", "["),
                     ("--batch-size", "0"), ("--encoding", "not-an-encoding"),
                     ("--base-url", "file:///tmp/"),
                     ("--base-url", "https://example.invalid/?q=x")):
            with self.subTest(args=args):
                self.run_tool(*args, code=2)
                self.assertFalse(self.output.exists())
        self.output = self.input/"nested-output"
        self.run_tool(code=2)
        self.assertFalse(self.output.exists())

    def test_symlinks_are_not_followed(self):
        self.html()
        outside = self.root/"outside"
        outside.mkdir()
        (outside/"secret.html").write_text("secret")
        (self.input/"linked-dir").symlink_to(outside, target_is_directory=True)
        (self.input/"linked.html").symlink_to(outside/"secret.html")
        self.run_tool()
        self.assertEqual(self.report()["summary"]["skipped_symlinks"], 2)
        self.assertEqual(len(self.items()), 1)
        self.assertNotIn("secret", self.items()[0].findtext(CONTENT))

    def test_no_html_rejected_before_creating_output(self):
        self.run_tool(code=2)
        self.assertFalse(self.output.exists())

    def test_special_file_rejected_without_opening_it(self):
        import os
        os.mkfifo(self.input/"pipe.html")
        self.run_tool(code=2)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
