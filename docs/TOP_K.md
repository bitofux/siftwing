# 稳定 top-k 合同

目标 `siftwing_top_k` 公开依赖 `siftwing_ranking`，公共权威为
[top_k.hpp](../include/siftwing/query/top_k.hpp)。`rank_top_k(Query,WeightedIndex,TopKLimits)`
先检查显式正 `max_top_k`，再检查查询请求条数，随后执行完整
[余弦排序](RANKING.md)，返回其前 `min(k,候选数)` 条。

`TopKLimits::ranking` 保留完整候选和评分预算；请求条数上限含端点，配置零拒绝，超限请求
不隐式缩小。应用可显式配置上限100，库允许完整正uint64范围；巨大k不预分配空间，命中少
不补齐。空结果、小k也不能绕过排序的正配置、FE_TONEAREST、候选与C*T准入。
错误为 `top_k.limits/top_k.top_k` 或原排序/候选诊断，后者message/context原样保留。
失败无部分结果，分配和未知异常上传。

输出独立拥有DocId与double，分数降序、精确同分DocId升序；分数逐位保持，没有epsilon
合并、重算或再次比较。OR未知忽略、AND未知空，全未知和空/移空索引成功空，移空Query
仍拒绝；查询重复次数的向量语义沿排序合同。输入同步稳定借用、不得与所有者移动/赋值/
析构并发，返回值在输入销毁后有效；外部修改后由调用者维护排序与分数性质。

本实现复用完整排序后缩容，以统一评分和错误合同。仍对全部候选计算分数，评分成本加
`O(C log(C+1))`排序、工作空间`O(T+C)`沿排序；额外缩容析构尾部，未引入按k的新容器。
返回少不证明查询更快、峰值内存为O(k)或CPU硬上限。堆选取会要求评分/排序合同拆分，
不在此接口中重复实现。可信资源、浮点确定性及跨CPU/libm边界沿排序；无摘要/推荐查询/
正式CLI/IO或共享缓存，也没有性能、OOM或并发专项验证。
