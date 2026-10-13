# codec 黄金与损坏输入验证

已有[协议](PROTOCOL.md)、[Buffer](BUFFER.md)、[拆帧](FRAME_DECODER.md)及
[编码与 Body 准入](FRAME_CODEC.md)的内存实现。本验证包检查这些实现的字节边界、
拒绝行为和组合性质；不更改协议或公共 API，也不执行 socket/EOF/业务字段处理。

## 三层验证

| 层 | 入口 | 所证明的性质与局限 |
|---|---|---|
| 独立参考与黄金 | `net.codec_reference` | Python `struct` 手工协议表产生期望，C++ probe只输出实际结果；全部16位类型、两类16位status、24头字节×256替换、黄金前缀/尾随、多点损坏优先级、长度/ID端点及原创Body分类 |
| 可重放损坏性质 | `net.codec_mutations` | 明确uint64 PRNG固定种子、单字节替换/截断/插入/删除及任意字节；消费、失败保持、拥有性、编码反解、字节再编码和响应关联；性质不能代替独立预期 |
| 覆盖反馈 fuzz | `siftwing_frame_codec_fuzzer` | Clang libFuzzer对入口和Buffer/decoder/codec实现插桩；ASan+UBSan运行有限输入并发现性质违例，不能证明不存在未知缺陷 |

黄金包含公开手工 PING/PONG，Python独立 `>4sBBHHHIQ` 布局再次核对，不由生产encoder
生成期望。所有测试使用原创字节，不读取私有语料。参考Body类别手工指定，保留已冻结的
64层/库数值限制、UTF8/原始NUL/转义和严格心跳政策，避免宽松的另一个JSON库掩盖差异。
两新增CTest使用显式失败检查，Release的NDEBUG不取消验证。

默认 Debug/Release 构建按README配置后执行：

```sh
ctest --test-dir build/debug --output-on-failure --no-tests=error -R '^net.codec_'
ctest --test-dir build/release --output-on-failure --no-tests=error -R '^net.codec_'
```

## 可选 libFuzzer

只在独立Clang构建中启用，不与GCC/普通Debug/Release目录混用：

```sh
cmake -S . -B build/fuzz -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DSIFTWING_ENABLE_SANITIZERS=ON -DSIFTWING_BUILD_CODEC_FUZZER=ON
cmake --build build/fuzz --target siftwing_frame_codec_fuzzer --parallel
mkdir -p build/fuzz/artifacts
python3 -B tests/integration/frame_codec_reference_test.py --write-fuzz-seeds build/fuzz/corpus
build/fuzz/siftwing_frame_codec_fuzzer build/fuzz/corpus \
  -seed=205 -runs=100000 -max_len=4096 -timeout=3 -rss_limit_mb=2048 \
  -artifact_prefix=build/fuzz/artifacts/
```

种子导出要求目标目录不存在，避免覆盖已有输入；corpus/artifacts应位于可重建构建目录
或另选外部路径，不能写原始私有输入。重复运行可复用**派生**corpus，已有最小反例须保留。
libFuzzer由Clang工具链提供，无新增运行库下载/系统安装；缺少相应运行时则构建失败。
选项要求BUILD_TESTING、Clang和SIFTWING_ENABLE_SANITIZERS同时开启，默认OFF。

专用fuzzer直接重编译三个生产`.cc`与性质实现，`-fsanitize=fuzzer-no-link`覆盖这些单元，
入口链接`-fsanitize=fuzzer`；ASan/UBSan沿项目构建选项启用。它不链接一份未插桩的codec
代替受测实现，也不改变默认生产目标的行为。普通三配置仍验证原生产库。

单次性质输入最多65536字节，每角色/预算流最多64帧，超出输入上限立即返回；完整Body的
协议1MiB上界另由独立参考测试覆盖。示例fuzz限制生成长度4096，长的初始深度种子仍保留。
`-runs`、`-timeout`、RSS预算是实验限制，不是生产系统硬资源保证；libFuzzer日志里的
cov/ft是编译器插桩指标，不等于协议分支穷尽、行覆盖百分比或业务覆盖率。

失败时输出性质标签并abort，fuzzer将当前输入保存到artifacts；复现方式：

```sh
build/fuzz/siftwing_frame_codec_fuzzer build/fuzz/artifacts/REPORTED_FILE -runs=1
```

崩溃、timeout、RSS或sanitizer报告均须保留原输入/命令/源码身份，定位后加明确预期的回归
再复测；不能吞异常、改Checker或删除失败语料来宣称通过。

## 证据边界

参考测试、固定PRNG回归和libFuzzer是三种不同证据。字节回环只证明组合，不能发现两端
共同写错；独立黄金与头oracle用于补足这一点。失败保持检查覆盖Buffer字节、容量、view
地址及重复调用结果，拥有性在输入存储被覆盖后检查。

有限运行不证明全部输入安全、真实OOM、任意线程并发、网络到达、请求调度、搜索/推荐
字段合法、吞吐或跨平台。EOF截断仍是上层合同，内存测试不得声称已关闭连接或发出响应。
