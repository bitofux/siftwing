#!/usr/bin/env python3
"""递归将本地HTML转换为含完整正文的RSS 2.0；只读输入，不抓取网页。"""

import argparse
import codecs
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from urllib.parse import quote, urlsplit


MIB = 1024 * 1024
AUTO_SELECTORS = (
    "main", '[role="main"]', "article", ".boostbook", ".refentry",
    ".chapter", "#content", ".contents", ".document", "body",
)
EXCLUDE_SELECTORS = (
    "script, style, nav, aside, footer, noscript, template, iframe, object, form",
    ".navheader, .navfooter, .spirit-nav, .toc, .sidebar",
)
INVALID_XML = re.compile(r"[^\x09\x0a\x0d\x20-\ud7ff\ue000-\ufffd\U00010000-\U0010ffff]")


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("必须是正整数")
    return number


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="本地HTML目录，递归读取.html/.htm")
    parser.add_argument("--output", type=Path, required=True, help="全新的输出目录；不覆盖已有文件")
    parser.add_argument("--base-url", help="输入目录对应的原网站URL前缀，例如https://www.boost.org/doc/libs/版本/")
    parser.add_argument("--selector", help="正文CSS选择器；默认自动选择；指定后不匹配即报告失败")
    parser.add_argument("--exclude-selector", action="append", default=[], help="额外移除的CSS选择器，可重复")
    parser.add_argument("--encoding", help="强制输入编码，例如utf-8或gb18030；默认按HTML声明自动检测")
    parser.add_argument("--batch-size", type=positive, default=100, help="每个RSS最多篇数，默认100")
    parser.add_argument("--max-feed-bytes", type=positive, default=8*MIB, help="每个RSS实际序列化字节上限，默认8 MiB")
    parser.add_argument("--max-html-bytes", type=positive, default=16*MIB, help="单个HTML读取上限，默认16 MiB")
    parser.add_argument("--title", default="本地文档语料", help="RSS频道标题")
    return parser.parse_args(argv)


def http_url(value):
    parsed = urlsplit(value)
    return parsed.scheme.lower() in ("http", "https") and bool(parsed.netloc)


def discover(root):
    """稳定遍历；不沿符号链接离开输入树，枚举失败不当成空目录。"""
    paths, skipped = [], []

    def fail(error):
        raise error

    for directory, dirs, files in os.walk(root, followlinks=False, onerror=fail):
        for name in sorted(dirs):
            path = Path(directory)/name
            if path.is_symlink():
                skipped.append({"path": path.relative_to(root).as_posix(), "status": "skipped_symlink"})
        dirs[:] = sorted(name for name in dirs if not (Path(directory)/name).is_symlink())
        for name in sorted(files):
            path = Path(directory)/name
            if path.suffix.lower() in (".html", ".htm"):
                if path.is_symlink():
                    skipped.append({"path": path.relative_to(root).as_posix(), "status": "skipped_symlink"})
                elif not path.is_file():
                    raise ValueError(f"HTML输入不是普通文件：{path.relative_to(root)}")
                else:
                    paths.append(path)
    return sorted(paths), skipped


def source_url(soup, path, root, base_url):
    if base_url:
        return base_url + quote(path.relative_to(root).as_posix(), safe="/")
    for selector, attribute in (
        ('link[rel~="canonical"][href]', "href"),
        ('meta[property="og:url"][content]', "content"),
    ):
        node = soup.select_one(selector)
        if node:
            value = str(node[attribute]).strip()
            if http_url(value):
                return value
    return path.as_uri()


def extract(path, root, args, BeautifulSoup, compile_selector):
    # 先限制读取量；DOM峰值内存仍受第三方解析器影响，不承诺进程内存硬上限。
    with path.open("rb") as stream:
        raw = stream.read(args.max_html_bytes + 1)
    if len(raw) > args.max_html_bytes:
        raise ValueError("HTML超过--max-html-bytes；未截断正文")
    if not raw.strip():
        raise ValueError("HTML为空")
    if args.encoding:
        # 显式编码严格解码，避免库猜测回退掩盖用户提供了错误编码。
        source = raw.decode(args.encoding)
    else:
        source = raw
    soup = BeautifulSoup(source, "lxml")
    if soup.contains_replacement_characters:
        raise ValueError("输入编码无法可靠解码，请使用--encoding指定编码")
    url = source_url(soup, path, root, args.base_url)
    page_title = soup.title.get_text(" ", strip=True) if soup.title else ""
    warnings = []
    for selector in (*EXCLUDE_SELECTORS, *args.exclude_selector):
        # 列表可能包含已被父节点移除的孩子；它们不再需要处理。
        for node in compile_selector(selector).select(soup):
            if node.parent is not None:
                node.decompose()
    selectors = (args.selector,) if args.selector else AUTO_SELECTORS
    body, chosen = None, None
    for selector in selectors:
        matches = soup.select(selector)
        if args.selector and len(matches) > 1:
            raise ValueError("--selector匹配多个区域，请选择它们共同的正文父元素")
        if len(matches) == 1:
            body = matches[0]
            chosen = selector
            break
    if body is None:
        raise ValueError("没有找到正文区域，请检查--selector")
    if not body.get_text(" ", strip=True):
        raise ValueError("正文区域没有可索引文字")
    heading = body.find("h1")
    title = heading.get_text(" ", strip=True) if heading else ""
    title = title or page_title or path.stem
    if not heading and not page_title:
        warnings.append("title_from_filename")
    if chosen == "body":
        warnings.append("body_fallback_check_navigation")
    # 保留HTML片段，让RSS Reader统一抽取文字；不会把C++尖括号误当成标签。
    # 删除注释，保留pre/code原有空白，不用get_text(strip=True)重写代码缩进。
    from bs4 import Comment
    for comment in body.find_all(string=lambda node: isinstance(node, Comment)):
        comment.extract()
    content = str(body)
    if INVALID_XML.search(title + content + url):
        raise ValueError("提取内容含XML 1.0禁止的字符，未静默替换")
    return {
        "path": path.relative_to(root).as_posix(),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "encoding": args.encoding or soup.original_encoding,
        "title": title, "url": url, "selector": chosen,
        "warnings": warnings, "content": content,
    }


def new_feed(args, root, FeedGenerator):
    feed = FeedGenerator()
    feed.title(args.title)
    feed.link(href=args.base_url or root.as_uri() + "/")
    feed.description("由本地HTML提取的完整正文；原始文件保持只读")
    return feed


def add_entry(feed, document):
    entry = feed.add_entry(order="append")
    entry.id(document["url"])
    entry.title(document["title"])
    entry.link(href=document["url"])
    # 用标准content:encoded保存完整片段，description只给频道阅读器一个提示。
    entry.content(document["content"])
    entry.description(document["title"])
    return entry


def write_batch(documents, output, index, args, root, FeedGenerator):
    """按实际XML字节检查并二分；不把超限单篇截成摘要。"""
    feed = new_feed(args, root, FeedGenerator)
    for document in documents:
        add_entry(feed, document)
    data = feed.rss_str(pretty=False)
    if len(data) > args.max_feed_bytes:
        if len(documents) == 1:
            document = documents[0]
            document.pop("content")
            document.update(status="failed", error="单篇RSS超过--max-feed-bytes；未截断正文")
            return index, []
        del feed, data
        middle = len(documents)//2
        index, left = write_batch(documents[:middle], output, index, args, root, FeedGenerator)
        index, right = write_batch(documents[middle:], output, index, args, root, FeedGenerator)
        return index, left + right
    name = f"feed-{index:05d}.xml"
    (output/name).write_bytes(data)
    for document in documents:
        document.pop("content")
        document.update(status="converted", rss=name)
    return index + 1, [{"path": name, "items": len(documents), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()}]


def convert(args, BeautifulSoup, FeedGenerator, compile_selector):
    if args.input.is_symlink():
        raise ValueError("输入目录不能是符号链接")
    root = args.input.resolve(strict=True)
    output = args.output.resolve()
    if not root.is_dir():
        raise ValueError("--input必须是目录")
    if output == root or root in output.parents or output in root.parents:
        raise ValueError("输入和输出目录不能相同或互相包含")
    if args.output.exists() or args.output.is_symlink():
        raise ValueError("输出路径已存在，请选择全新目录；不会覆盖已有文件")
    if args.base_url:
        parts = urlsplit(args.base_url)
        if not http_url(args.base_url) or parts.query or parts.fragment:
            raise ValueError("--base-url须是无查询参数和片段的HTTP(S)目录URL")
        args.base_url = args.base_url.rstrip("/") + "/"
    if args.encoding:
        codecs.lookup(args.encoding)
    for selector in (*EXCLUDE_SELECTORS, *args.exclude_selector, *((args.selector,) if args.selector else ())):
        compile_selector(selector)
    if not args.title.strip() or INVALID_XML.search(args.title):
        raise ValueError("频道标题不能为空或含XML禁止字符")
    paths, skipped = discover(root)
    if not paths:
        raise ValueError("没有找到普通.html/.htm文件")
    output.mkdir(parents=True, exist_ok=False)
    records, pending, feeds = [], [], []
    index, pending_bytes = 1, 0
    for number, path in enumerate(paths, 1):
        try:
            document = extract(path, root, args, BeautifulSoup, compile_selector)
            records.append(document)
            pending.append(document)
            pending_bytes += len(document["content"].encode("utf-8"))
        except (OSError, ValueError, UnicodeError) as error:
            records.append({"path": path.relative_to(root).as_posix(), "status": "failed", "error": str(error)})
            print(f"转换失败：{path.relative_to(root)}：{error}", file=sys.stderr)
        if len(pending) >= args.batch_size or pending_bytes >= args.max_feed_bytes:
            index, batch = write_batch(pending, output, index, args, root, FeedGenerator)
            feeds.extend(batch)
            pending = []
            pending_bytes = 0
        if number % 100 == 0:
            print(f"已处理 {number}/{len(paths)} 个HTML", file=sys.stderr)
    if pending:
        index, batch = write_batch(pending, output, index, args, root, FeedGenerator)
        feeds.extend(batch)
    converted = sum(record["status"] == "converted" for record in records)
    failed = len(records) - converted
    report = {
        "schema_version": 1, "input": str(root),
        "options": {key: value for key, value in vars(args).items() if key not in ("input", "output")},
        "summary": {"html_files": len(paths), "converted": converted, "failed": failed,
                    "skipped_symlinks": len(skipped), "rss_files": len(feeds),
                    "rss_bytes": sum(feed["bytes"] for feed in feeds)},
        "feeds": feeds, "documents": records + skipped,
    }
    (output/"report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"完成：成功 {converted}，失败 {failed}，跳过链接 {len(skipped)}，RSS {len(feeds)} 个")
    print(f"输出：{output}\n清单：{output/'report.json'}")
    if converted > 10000 or report["summary"]["rss_bytes"] > 128*MIB:
        print("提示：输出超过当前Siftwing单次建库的10000篇或128 MiB预算，请分目录建库。", file=sys.stderr)
    if any(feed["bytes"] > 16*MIB for feed in feeds):
        print("提示：存在超过Siftwing单输入16 MiB预算的RSS，请减小--max-feed-bytes。", file=sys.stderr)
    return 1 if failed else 0


def main(argv=None):
    args = arguments(argv)
    try:
        from bs4 import BeautifulSoup
        from feedgen.feed import FeedGenerator
        from soupsieve import compile as compile_selector
        from soupsieve.util import SelectorSyntaxError
        # 显式检查所选解析器，--help仍可在未安装依赖时使用。
        import lxml.etree  # noqa: F401
    except ImportError:
        print("缺少依赖，请先运行：python -m pip install -r tools/requirements-html-rss.txt", file=sys.stderr)
        return 2
    try:
        return convert(args, BeautifulSoup, FeedGenerator, compile_selector)
    except (OSError, ValueError, LookupError, SelectorSyntaxError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("已中断；输出可能不完整，请保留现场并换新目录重试。", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
