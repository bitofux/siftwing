# 第三方依赖

当前TXT/RSS适配器、HTML辅助及共用文本规范化使用 [utfcpp](https://github.com/nemtrif/utfcpp) 4.0.6（也称utf8cpp），
负责严格UTF-8验证和码点遍历。它是头文件库，许可证为BSL-1.0；
许可原文保留在 [utfcpp-LICENSE.txt](../third_party/licenses/utfcpp-LICENSE.txt)。

CMake通过FetchContent取得官方tag归档：

```text
https://codeload.github.com/nemtrif/utfcpp/tar.gz/refs/tags/v4.0.6
SHA-256: 6920a6a5d6a04b9a89b2a89af7132f8acefd46e0c2a7b190350539e9213816c0
```

首次配置需要网络，归档与源码保存在所选构建目录的`_deps/`，不安装到系统目录。
固定哈希用于检测下载字节变化；归档获取失败或哈希不匹配时配置失败，不静默换版本。
项目只使用其头文件，通过`siftwing_utf8cpp`的SYSTEM INTERFACE include声明依赖，
不执行上游安装规则或编译上游测试，也不修改上游源码。公共TXT/RSS/HTML/规范化头文件不暴露第三方类型。

离线构建可以显式设置`-DFETCHCONTENT_SOURCE_DIR_UTF8CPP=/ABS/VERIFIED/utfcpp-4.0.6`。
该CMake覆盖绕过下载哈希检查，使用者须独立确认版本、完整性和许可；不能把任意本地目录
冒称已验证的4.0.6归档。组件升级需重新核对接口、许可与各适配器/规范化行为。M1.10复用该库校验/遍历UTF-8；
ASCII小写及Unicode15.1 White_Space固定规则由项目实现，不新增Unicode大小写/规范等价库。

## HTML片段抽取：Lexbor

HTML片段抽取使用 [Lexbor](https://github.com/lexbor/lexbor) **3.0.0**，承担HTML5
片段解析、实体还原及畸形标签恢复。项目使用其C接口，通过C++17 RAII管理解析器/文档；
公共头文件不暴露Lexbor类型。它不负责RSS XML读取、浏览器渲染、正文识别或网络抓取。

官方发布归档与固定校验值：

```text
https://codeload.github.com/lexbor/lexbor/tar.gz/refs/tags/v3.0.0
SHA-256: eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2
```

上游主许可证为Apache-2.0；core中部分数值转换代码附BSD-2-Clause条款，完整原文均在
[lexbor-LICENSE.txt](../third_party/licenses/lexbor-LICENSE.txt)中。原样保留上游
[lexbor-NOTICE.txt](../third_party/licenses/lexbor-NOTICE.txt)。项目自身代码继续MIT；
分发包含Lexbor的静态链接程序时须随附上述许可证、归属声明及NOTICE。
项目不修改上游源码，也不将上游代码改标为MIT。

CMake使用FetchContent下载并校验固定归档，在构建目录保存源码；按上游模块依赖只编译
`html`、`dom`、`core`、`ns`、`tag`和POSIX端口，形成`siftwing_lexbor`静态目标。
这些模块无额外下载依赖，仅链接系统`libm`。第三方代码使用C99，项目自身仍使用C++17。
接入不运行上游安装、示例、测试、CSS/渲染模块，也不系统安装库。ASan/UBSan配置同时
插桩Lexbor的C代码和项目C++代码，五类项目警告不施加到未修改的第三方源码。

首次配置需要网络；缺失、下载失败或哈希不匹配会使配置失败，不静默换库。
离线可设置`-DFETCHCONTENT_SOURCE_DIR_LEXBOR=/ABS/VERIFIED/lexbor-3.0.0`，该覆盖绕过
下载校验，使用者须独立核对归档版本、源码完整性、许可证及NOTICE。

抽取函数使用独立解析状态，不修改库的全局内存钩子。输入预算在解析前检查，token和
元素栈预算在解析中检查，DOM节点/深度在输出前检查，输出在字符串增长前检查；这不保证
整个进程的内存/CPU硬上限。HTML修复诊断保留为`HtmlText::recovered`，空正文与失败分别表示。
原创行为测试、公开黄金RSS字段集成及公共头编译合同覆盖实体、段落、非正文过滤、恢复、
拥有性和限额；升级依赖须重新验证这些规则及模块依赖树。

## RSS XML结构读取：tinyxml2

RSS适配器使用[tinyxml2](https://github.com/leethomason/tinyxml2) **11.0.0**，负责内存中
XML DOM构造、直接Text/CDATA和属性读取；HTML正文字段仍由Lexbor处理。公共RSS接口不暴露
第三方类型。官方tag归档及固定校验值：

```text
https://codeload.github.com/leethomason/tinyxml2/tar.gz/refs/tags/11.0.0
SHA-256: 5556deb5081fb246ee92afae73efd943c889cef0cafea92b0b82422d6a18f289
```

许可证为Zlib，完整原文保留在[tinyxml2-LICENSE.txt](../third_party/licenses/tinyxml2-LICENSE.txt)。
分发含该静态库的程序须保留相关许可证及归属；项目自身继续MIT，不修改或改标上游代码。
CMake FetchContent校验下载，只编译`tinyxml2.cpp`形成`siftwing_tinyxml2`静态目标，SYSTEM
include声明头路径；不运行上游安装/工具/示例/测试，没有额外下载依赖，也不系统安装。
项目C++17和禁用扩展同时适用于该目标；五类项目警告不施加到第三方实现，ASan/UBSan配置
插桩该实现。所有解析器实例按次独立，栈对象管理DOM生命周期。

离线可设置`-DFETCHCONTENT_SOURCE_DIR_TINYXML2=/ABS/VERIFIED/tinyxml2-11.0.0`，覆盖绕过
CMake下载哈希检查，使用者须独立核实源码版本、完整性和许可证。下载失败/哈希不匹配使
配置失败，不静默换版本。

库的Parse成功不能证明本项目XML支持合同：tinyxml2容许部分声明/实体/命名空间情况，
项目另做UTF-8、XML1.0字符/实体/声明、DTD禁用和命名空间检查。RSS支持范围和正文选择
由项目承担，不声称任意XML均可接入。tinyxml2内部元素深度限制500，项目XML节点/深度预算
在完整建树后检查；输入字节预算在解析前实施，但不承诺进程内存/CPU硬上限。业务不调用
LoadFile或联网，也不加载外部实体。升级须重新验证上述边界、RSS行为与许可。

## 计划中的依赖

cppjieba及SimHash组件属于后续模块规划，当前构建不引入它们。
