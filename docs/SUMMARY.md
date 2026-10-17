# 动态摘要合同

摘要组件（链接别名`siftwing_summary`）实现复用`siftwing_query`，实现私有复用既有utfcpp；无新第三方。
公共权威为[summary.hpp](../include/siftwing/query/summary.hpp)。
`make_summary(Query,原正文,SummaryOptions,SummaryLimits)`返回独立拥有的原文连续窗口。
调用方在[top-k](TOP_K.md)后，用同一快照的DocId取得正文再调用；库不访问Reader、标题或IO。

正文与全部唯一查询键仅ASCII A—Z折小写用于定位，其他Unicode标量精确匹配。选择正文中
最早出现的词键子串；允许重叠，无词边界保证（`he`可匹配`the`）。不重新分词、不做NFC、
完整Unicode大小写折叠或空白折叠，与查询TF次数、AND/OR和k无关；未命中取开头。
这不是索引词位或分词命中的重建：查询键可能在显示原文找不到，也可能以子串命中。

`window_scalars`默认160，`leading_context_scalars`默认40；窗口须正，前文长度允许0但须
小于窗口。N为正文标量数，W=min(N,window_scalars)，最早命中h时窗口起点为
`min(max(0,h-leading_context_scalars),N-W)`，无命中为0。靠近末端向左补足窗口，不越界。
窗口可以短于命中词，仅保证命中起点在窗口；标量边界不保证字素簇/组合字符/emoji序列完整。

返回`Summary::text`保留原文大小写、换行和空白，`source_byte_offset`是原文字节偏移；
`matched`记录完整正文是否存在词键子串，`truncated_before/after`记录未返回的前后缀。
不加省略号或任何标记。空/缺失正文以空串输入，成功空且偏移0/三布尔false，不回退到标题；
空白正文仍按原值截取。[高亮组件](HIGHLIGHT.md)可消费text生成安全HTML及相对text的原字节
区间，不修改正文；截短窗口可能丢完整词，高亮不依赖matched。JSON转义由序列化层负责。

五项显式正预算依次覆盖原正文总字节、唯一查询词数、累计词键字节、完整匹配N*Q及请求
窗口上限；Q是全部唯一键累计标量数，重复TF不重复计量。检查顺序为五正配置→选项→有效
Query→正文字节→完整严格UTF-8→NUL→词数/键字节→N*Q。含端点，空正文仍检查配置和
查询预算，早期命中或小窗口不能绕过完整校验/工作预算。context为
`summary.limits/options/query/content_bytes/utf8/nul/query_terms/query_bytes/work`；
编码/NUL诊断含0基原字节位置。移空Query拒绝，分配/未知异常上传，失败无部分摘要。

输入同步稳定借用，不与所有者写入/移动/析构并发；输出无借用逃逸，输入销毁后仍有效。
输出可修改，修改者负责维持文本/偏移/标记一致性。最坏匹配O(N*Q)，验证/解码按完整正文
和键字节计，额外空间O(N+Q)，输出最多4W字节；预算不是硬内存/CPU保证。无共享缓存，
未进行性能、真实OOM、并发或业务质量评估。

真实CTest注册`query.summary`、`query.summary_reference`、`query.summary_snapshot`与
`query.summary_compile_contract`。独立Python严格解码/子串查找/枚举合法窗口核原字节和
元数据，原创TXT/RSS在英文和中文管线经Writer/Reader与top-k接线；测试探针不是正式CLI。
