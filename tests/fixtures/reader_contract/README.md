# 读取合同原创夹具

本目录的 scenarios.h 在本项目中原创，按仓库 MIT 许可证发布。夹具以 C++ 拥有型值
人工指定来源、标题、正文、诊断和未覆盖范围，不依赖老师答案或私有语料。

- complete 创建显式身份的完整记录；输入 path/ordinal/id/text 均由测试手工指定。
- diagnostic 创建无载荷的无文字、拒绝或失败结果。
- partial 固定“纸船”、第一段正文、paragraphs: 2 未覆盖范围；策略接纳时必须保留这些原值，
  策略拒绝时必须清除文档且保留诊断，不把部分正文称为完整文档。

document.reader 使用测试专用 ScriptReader 交付这些结果，证明共用层的来源/顺序/上限/
报告/停止合同。它不解析 TXT、RSS 或 JSONL，不作为生产读取器发布。边界值采用短文本，
让 byte limit 在等于及大于1字节时可手算；大量字节测试只在外部构建/临时内存中产生。

此外测试只读已有 golden/txt/en-river.txt 和 empty.txt；对原始字节的独立预期为
`A paper boat drifts beside the river gate.\nThe lantern is green.\n`（65字节）与0字节。
这项对照不冻结未来TXT标题回退、BOM/换行或编码抽取规则。输入/夹具均不会写回。
