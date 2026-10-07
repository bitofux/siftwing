# 第三方依赖

当前TXT适配器使用 [utfcpp](https://github.com/nemtrif/utfcpp) 4.0.6（也称utf8cpp），
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
不执行上游安装规则或编译上游测试，也不修改上游源码。公共TXT头文件不暴露第三方类型。

离线构建可以显式设置`-DFETCHCONTENT_SOURCE_DIR_UTF8CPP=/ABS/VERIFIED/utfcpp-4.0.6`。
该CMake覆盖绕过下载哈希检查，使用者须独立确认版本、完整性和许可；不能把任意本地目录
冒称已验证的4.0.6归档。组件升级需重新核对接口、许可与TXT行为。

cppjieba、tinyxml2及SimHash组件属于后续模块规划，当前构建不引入它们。
