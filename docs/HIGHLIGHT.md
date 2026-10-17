# 关键词高亮合同

高亮组件（链接别名`siftwing_highlight`）实现复用`siftwing_query`，实现私有复用固定utfcpp，无新第三方。
公共权威为[highlight.hpp](../include/siftwing/query/highlight.hpp)。
`make_highlight(Query,文字,HighlightLimits)`返回拥有型`Highlight`：安全HTML正文片段`html`
与相对本次输入原UTF-8字节的`spans`（`byte_offset/byte_length`）。

调用方在[top-k](TOP_K.md)取得同快照正文，再生成[摘要](SUMMARY.md)，将`Summary.text`
交给高亮。高亮不读取Reader/索引或摘要元数据；偏移相对摘要文字，不是完整正文或HTML。
完整正文偏移由调用方结合`Summary.source_byte_offset`解释。截短摘要可能不含完整词键，
摘要`matched=true`不保证高亮有命中。

全部唯一保留查询词参与，与次数、AND/OR和k无关，索引未知词也可以在显示文字命中。
匹配仅ASCII A—Z折小写，其他Unicode标量精确子串，穷举全部起点并保留原文大小写。
重叠及相邻命中取并集，返回正长度、有序、严格分离区间，每个只加一对固定`<mark>`标签。
例如`ab/bc`对`abc`得到`<mark>abc</mark>`，`a/b`对`ab`也只生成一个区间。
无词边界（`he`可匹配`the`）、NFC、完整casefold或字素簇保证，不重新分词。

匹配发生在原文，之后完整转义`& < > " '`为`&amp; &lt; &gt; &quot; &#39;`，最后添加程序
生成的固定标签。原有HTML/实体都是字面输入，不解析、不信任、不允许自定义标签。
无命中仍转义，空文字成功空HTML/空区间。例如查询`中国/中国人`、文字`<中国人>&`
得到`&lt;<mark>中国人</mark>&gt;&amp;`，原字节区间为`{1,9}`。

输出仅用于HTML正文文本位置，不用于属性、URL、JS、CSS，也不是HTML净化器。
输出作为JSON字段时由JSON序列化层另外转义；再次把HTML输入本函数会再次转义，不幂等。
原始文字、查询、快照与评分都不改变；显示层不把正文转换回索引文本。

六项显式正uint64预算依次是原文字节、唯一查询词数、累计键字节、完整匹配`N*Q`、合并
区间数及最终HTML字节；N为文字标量数，Q为全部唯一键累计标量数，重复TF不重复计量。
输出预算含实体与每对13字节标签，区间预算计并集而非原始命中次数；含端点。
检查顺序为六正配置→有效Query→原字节→完整严格UTF-8→NUL→词数/键字节→工作→
合并区间数→最终HTML字节。早期命中/无命中不绕过全文校验和工作准入，空文字仍检查
查询预算。非法尾部不会被输出超限掩盖，区间数超限优先于输出字节超限。

context为`highlight.limits/query/text_bytes/utf8/nul/query_terms/query_bytes/work/spans/output_bytes`；
编码/NUL诊断含0基原字节位置。移空Query拒绝，failure无部分结果；分配/未知异常上传，
局部RAII释放。输入/查询/预算同步稳定借用，不得并发写或与所有者移动/析构并发。
输出独立拥有、可复制/移动/修改，外部修改者负责维持HTML与区间关系。

最坏O(N*Q+B+H)，额外O(N+Q+S+H)，B原字节、S合并区间数、H输出字节；预算不是硬CPU/
内存限。未测性能、真实OOM、并发或业务质量，不保证跨字素簇展示完整。

真实CTest为`query.highlight`、`query.highlight_reference`、`query.highlight_snapshot`及
`query.highlight_compile_contract`。Python用严格Unicode解码、逐词find后的字符覆盖集合、
标准`html.escape`及HTMLParser独立核验区间/HTML/显示原文；原创TXT/RSS经英文中文管线、
Writer/Reader、top-k与摘要接线。测试探针不是正式CLI，测试词键工具复用既有显式分隔辅助头。
