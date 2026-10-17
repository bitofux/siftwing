# 本地 HTML 批量转换为 RSS

`tools/html_to_rss.py` 使用 BeautifulSoup + lxml 提取本地 HTML，再由 feedgen 生成
Siftwing 已有 RSS Reader 可以读取的 RSS 2.0。每个 HTML 对应一个 `item`，完整正文
以 HTML 片段存入 `content:encoded`，保留代码块、缩进和表格结构；不截断成摘要。

该脚本只用于离线准备语料，不是 C++ 服务依赖，也不改变服务的原生输入格式。
它不下载网页、不执行 JavaScript、不加载图片或其他外部资源，不改写原始 HTML。

## 安装一次

从仓库根目录执行，要求 Python 3.10+：

```bash
python3 -m venv ~/.local/share/siftwing/html-rss-venv
source ~/.local/share/siftwing/html-rss-venv/bin/activate
python -m pip install -r tools/requirements-html-rss.txt
```

后续新终端只需重新运行 `source` 那一行。如果 Ubuntu 提示无法创建虚拟环境，
先通过系统包管理器安装与你的 Python 对应的 `venv` 支持。

## 一条命令转换

```bash
python tools/html_to_rss.py --input /path/to/boost-docs --output /path/to/new-rss
```

递归读取 `.html` / `.htm`，扩展名不区分大小写。输出目录必须不存在，且不能与
输入目录相同或互相包含；重复转换使用新的输出目录，避免混入旧 RSS 或覆盖文件。
跳过符号链接文件及目录。转换时保持输入目录不变，不与其他写入程序并发运行。

输出示例：

```text
new-rss/
├── feed-00001.xml
├── feed-00002.xml
└── report.json
```

默认每个 RSS 最多 100 篇，且实际序列化后不超过 8 MiB；超过字节上限的批次会
继续拆分，单篇仍超限则报告失败，不截断正文。单个 HTML 默认最多读取 16 MiB。

## 保留原网页 URL

若知道输入目录在原网站上的对应位置，使用 `--base-url`：

```bash
python tools/html_to_rss.py \
  --input /path/to/boost_1_XX_0 \
  --output /path/to/new-rss \
  --base-url https://www.boost.org/doc/libs/1_XX_0/
```

这里的版本和目录为占位示例，需替换为实际版本，并确认对应网页地址可用。
假如输入文件是 `libs/asio/doc/index.html`，条目 URL 就是
`https://www.boost.org/doc/libs/1_XX_0/libs/asio/doc/index.html`。

URL 按以下优先级生成，仅保存元数据，不访问 URL：

1. 指定 `--base-url`：与相对文件路径组合，空格及中文进行 URL 编码。
2. 未指定：优先采用页面中绝对 HTTP(S) `canonical`，其次 `og:url`。
3. 都没有：使用本地文件的 `file://` URI。它适合本机学习，不能作为网站公开链接。

RSS 与 `report.json` 可能包含本地路径和文档全文；生成物按输入资料的隐私与许可要求保管。

## 调整正文区域

默认依次尝试唯一匹配的 `main`、`[role="main"]`、`article`、`.boostbook`、
`.refentry`、`.chapter`、`#content`、`.contents`、`.document`，最后回退到 `body`。
使用 `body` 会在清单中标记 `body_fallback_check_navigation`，提醒检查导航噪声。
标题优先正文内的首个 `h1`，其次页面 `title`，最后文件名。

不同 Boost 库可能使用不同页面模板，自动规则无法保证所有页面都准确。先转换小目录，
检查清单及少量 RSS 正文，再转换大目录；必要时按模板分别转换。

例如正文位于 `<div id="content">`，还需要去除 `.advertisement`：

```bash
python tools/html_to_rss.py \
  --input /path/to/html \
  --output /path/to/new-rss \
  --selector '#content' \
  --exclude-selector '.advertisement'
```

`--selector` 必须恰好匹配一个区域，缺失或多匹配都报告该页失败，不偷偷回退到其他区域。
`--exclude-selector` 可重复使用，是在默认清理规则之外追加移除区域。
默认删除脚本、样式、导航、侧栏、页脚、表单及常见 Boost 导航/目录节点；清理规则是启发式的，
不用于精确还原网页排版。代码示例作为 HTML 片段保存，Siftwing 建库时仍应用自身的文本抽取规则。

## 其他参数与结果

```bash
python tools/html_to_rss.py --help
```

| 参数 | 默认值 | 用途 |
|---|---|---|
| `--batch-size` | `100` | 每个 RSS 最多篇数 |
| `--max-feed-bytes` | `8388608` | 单个 RSS 的实际 XML 字节上限 |
| `--max-html-bytes` | `16777216` | 单个 HTML 的读取上限 |
| `--encoding` | 自动检测 | 强制输入编码，如 `utf-8`、`gb18030`；错误编码严格报错 |
| `--title` | `本地文档语料` | RSS 频道标题 |

清单包含成功/失败/跳过数量、每页来源路径、输入 SHA-256（成功完成提取时）、
检测编码、标题、URL、正文选择器、警告、对应 RSS，以及输出 RSS 的大小和 SHA-256。
频道的 `lastBuildDate` 由 feedgen 写入转换时间，因此不同时间重复转换不承诺 XML 字节一致；
条目顺序按相对路径稳定排列，不虚构原文发布时间。

- 退出 `0`：全部普通 HTML 转换成功，符号链接可能被跳过。
- 退出 `1`：有页面失败；成功页面仍生成 RSS，详细原因见清单。
- 退出 `2`：参数、依赖或文件系统等错误；可能有不完整输出，不能当作完整转换结果。
- 退出 `130`：用户中断，输出可能不完整。

当前 Siftwing 单次建库仍有每支最多 10000 篇、累计输入 128 MiB、单输入 16 MiB 等预算。
RSS 分片不绕过总预算；大语料需选择子目录/分批建库。脚本超过篇数或累计输入预算时会提示，
但生成 RSS 成功不代表整个语料已通过建库、去重或检索质量验证。

## 验证脚本

使用已安装依赖的虚拟环境，独立注册并运行 CTest，不向 C++ 默认构建增加 Python 库依赖：

```bash
cmake -S tools/tests -B build/html-rss-tests \
  -DPython3_EXECUTABLE="$(command -v python)"
ctest --test-dir build/html-rss-tests --output-on-failure --no-tests=error
```

测试使用原创临时 HTML，验证正文/代码/实体、URL、分片、编码、失败报告与不覆盖输入输出。
