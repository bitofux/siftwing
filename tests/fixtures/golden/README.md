# 原创黄金测试数据 v1

SPDX-License-Identifier: MIT

这些短篇虚构文本与 RSS 记录于 2026-10-06 在 Codex 协助下为 Siftwing 原创编写，未复制课程资料、私有语料、网站或第三方数据集。本目录全部测试数据使用项目 MIT 许可证，副本见 LICENSE。

这些数据为后续模块提供小型、可复现的输入基线，不实现读取、文本规范化、去重、索引或搜索。manifest.json 中的 item 字段表示 XML 元素的原始文本，包含未经处理的 HTML 字符串；不冻结抽取、回退、分词、文档身份或排名规则。

| 输入 | 用途 |
| --- | --- |
| txt/en-workbench.txt | 原创英文文本，两行均以 LF 结束 |
| txt/en-river.txt | 第二篇英文文本，含共有词和不同细节 |
| txt/zh-workbench.txt | 原创中文 UTF-8 文本 |
| txt/zh-river.txt | 第二篇中文 UTF-8 文本 |
| txt/en-workbench-copy.txt | en-workbench.txt 的逐字节副本 |
| txt/empty.txt | 零字节空文件 |
| rss/original.xml | 三个 RSS item，覆盖带命名空间的 CDATA、转义 HTML/实体和缺失元数据 |

全部非空输入均为无 BOM 的 UTF-8，使用 LF 换行，并以 LF 结束。RSS URL 使用保留的 .invalid 域名，仅作为测试标识；测试不会访问这些地址。不使用原始私有输入或已保存的私有观测清单。

manifest.json 保存稳定排序的输入列表、准确字节大小与 SHA-256、重复和空文件关系，以及人工指定的原始 RSS 字段。数据 ID 是测试标签，不是生产 DocumentId。主动修改测试数据时，先审查原始字节与独立预期字段，再更新清单；不得从被测实现重新生成预期。

fixtures.integrity 通过 CTest 核对该基线，并在临时目录中检查损坏、缺失、多余、符号链接、不一致和错误许可的副本。测试只读源数据，不修改这些文件。它验证声明的字节和结构预期，不能独立证明作者身份或未来搜索的正确性；后续模块按各自批准的合同增加行为黄金输出。
