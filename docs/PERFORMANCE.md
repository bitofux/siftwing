# 可复现本机性能基准

`tools/v3_benchmark.py`编排正式`siftwing-indexer`和`siftwing-server`，由独立C++17
发生器发起TCP闭环请求。它保存测量条件、成功与失败、每次成功响应的原始纳秒样本，
以及CPU、RSS和缓存统计。性能结果应与源码和程序身份一起引用。

## 构建与复跑

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DSIFTWING_BUILD_BENCHMARKS=ON
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure --no-tests=error
python3 -B tools/v3_benchmark.py \
  --server build/release/siftwing-server \
  --indexer build/release/siftwing-indexer \
  --loadgen build/release/siftwing_benchmark_loadgen \
  --output /ABS/EXISTING_PARENT/NEW_RESULT --seconds 5 --repeats 3
```

输出父目录须已存在，结果目录必须全新。工具只写该目录，不清空旧结果或系统页缓存；
失败保留原件并停止。先用`--seconds 1 --repeats 1 --smoke`验证流程；烟测的短窗口
用于工具验收，不与正式性能矩阵合并。每次测量前停止本项目的构建与其他测试。
Debug、ASan/UBSan和TSan用于验证工具，不用于性能数字。

使用当前源码构建生产程序，再运行工具；工具会保存Git HEAD、公开源文件SHA、工作树状态
和三个程序的SHA。仅观察源码与程序哈希不能证明任意外来二进制的构建来源，须另保留
构建命令、flags和源码对应关系。源码/程序在本轮运行中变化会使本轮失败。

## 固定输入与负载

- 默认2,000篇原创合成英文RSS，Random种子20261016；每篇从256个八字母词中选40个，
  唯一标题/链接，搜索SimHash距离0。实际保留文档数读取建库输出，不能由输入篇数推断。
- 查询池有64个不同词；300个位置的固定循环中，240次访问前4个热门词、60次分别访问
  其余词，然后固定种子打乱。各连接有固定循环偏移，属于相关的确定性80/20访问。
- 每线程一条持久连接、一次在途、无重试；连接、SERVER_INFO和逐连接5次预热在计量屏障外。
  SEARCH/SUGGEST取top_k=5，响应非空；PING固定空对象。
- 固定loopback、一个调用线程Reactor、4个worker、等待容量128，总连接/Completion容量各256；业务期限关闭，发生器每次交换总期限5秒。
  日志级别info、文件64KiB/1份归档，保留最终各族指标；日志编码、同步写入与轮转开销包含在结果中。
  这是明确的基准配置，日志轮转参数与正式入口默认值不同。

主矩阵3轮，每窗至少5秒（可指定1..60秒），PING使用1/8/32连接、关闭缓存；
SEARCH/SUGGEST在相同并发和请求分布下比较关闭缓存与充分预热LRU。每个窗口新建服务进程，
每轮固定种子打乱顺序，保留全部窗口和范围。

热缓存容量256、charge预算8MiB、单Body上限256KiB；开始前访问全部64个唯一业务键。
关闭缓存执行相同预热。另测两业务在8连接、容量16时的淘汰场景；同一访问分布下允许混合命中，
由最终真实计数验证发生淘汰。每轮另用新进程、新连接各测64个不同键的首次填充，缓存关闭作为对照。
首次填充没有业务预热，不表示冷磁盘或冷OS页缓存。

默认完整运行共63窗口：45主窗口、6淘汰窗口、12首次填充窗口。
首次填充按固定请求数结束，其实际时长可短于`--seconds`；其余窗口包含最后请求排空，
实际时长可能略长。日志中的缓存计数包含进程预热与窗口请求，不能直接当作纯窗口命中率。

## 结果准入与统计口径

发生器独立编码24字节大端帧，检查magic、版本、头长度、type、flags、status、关联ID和Body预算，
完整接收后解析UTF-8/JSON，验证schema/版本/命令，并与本轮正式cache-off服务取得的完整Body逐字节比较。
SERVER_INFO还核对build身份。后者证明配置间的结果一致性，独立排名正确性由既有业务测试验证。

延迟从请求编码前到完整参考准入后；建连、INFO和预热不计入延迟。成功吞吐分母为屏障开启至
最后线程完成请求的实际时间，包含末尾排空。ERROR、超时、截断、错ID、非法JSON或内容不符均不计成功。
原始每线程样本在`loadgen.stdout`，分位使用nearest-rank；汇总取“三轮各窗口指标的中位数”，
同时给出吞吐最小/最大值，不平均分位数，也不冒充合并所有样本的分位数。

每50ms保存服务`/proc/PID/stat`和status资源样本。服务CPU百分比以一个逻辑CPU为100%，可大于100%；
资源窗口含发生器启动、连接、预热和末尾采样间隔，与纯延迟窗口不同。RSS与VmHWM是进程观察，
LRU charge不能当作RSS，峰值也不是内存上界或泄漏证明。客户端CPU和峰值RSS独立记录。

## 产物与证据边界

结果目录包含环境/可见cgroup、配置、输入与快照SHA、源码/程序身份、每窗命令/退出、
服务输出/轮转日志、最终指标、资源原样本、每请求延迟、`results.json`和`summary.json/.csv`。
性能汇总只有全流程成功后才发布`complete.json`；保留失败目录，不能只挑最快轮。

`benchmark.loadgen_contract`由真实CTest注册，使用原创伪服务验证并发成功、分段帧、各类坏头、
非法UTF-8/JSON、截断、ERROR、少量进展后的总期限和build不符，并从原样本核对成功计数与分位。
它验证测量工具，不替代生产服务的正确性回归。

本基准是同机闭环负载，发生器也占CPU，会自限速；不能推成开放到达下的排队/过载分布。
合成英文、小词表、固定热门度和loopback不代表真实中文、任意索引规模、跨机RTT或生产容量。
机器/虚拟化、宿主争用、CPU亲和性、cgroup、日志profile与采样长度都要随数字说明；
当前工具不修改系统配置，不自动调优、切快照或定位锁热点。故障/持续正确性验证见
[V3可靠性验证](V3_RELIABILITY.md)，服务合同见[在线搜索](ONLINE_SEARCH.md)。

## 单Reactor迁移边界

当前编排工具显式使用总max_connections=256、completion_capacity=256和workers=4，不再传已退役的subs。每轮server-topology.json保存启动schema=2与声明拓扑；线程/RSS采样不是完整I/O归属证明。迁移前归档数据属于Main/Sub架构，不重写、不作为当前成绩；smoke只验证工具链，不能宣称吞吐改善。
