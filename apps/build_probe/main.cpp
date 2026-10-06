/*
 * PROJECT : SIFTWING
 * FILE    : main.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-06
 * BRIEF   : 提供构建基线使用的字节计数命令行入口，不是搜索服务入口
 */

#include "siftwing/bootstrap/byte_probe.h"

#include <iostream>
#include <string_view>

/**
 * @brief 校验命令行参数，运行字节计数探针并报告结果
 *
 * argv[1] 必须是一个非 NUL 命令行字节，argv[2] 是由 C 字符串表达的待扫描字节；命令行
 * 本身无法表达嵌入 NUL，库级测试单独覆盖该边界。
 *
 * @param[in] argc
 *     参数数量，必须为 3。
 * @param[in] argv
 *     进程启动环境提供的参数数组；本函数只在 argc 合法后读取 argv[1] 和 argv[2]。
 *
 * @retval 0
 *     计数及换行已写入 stdout，显式 flush 后流状态正常。
 * @retval 1
 *     输出或 flush 后 stdout 处于失败状态。
 * @retval 2
 *     参数数量错误或 needle 不是恰好一个字节；usage 写入 stderr。
 *
 * @note 入口不持久化输入，也不提供索引、查询或网络服务能力。
 */
int main(int argc, char* argv[]) {
    if (argc != 3 || std::string_view{argv[1]}.size() != 1) {
        std::cerr << "usage: siftwing_build_probe <single-byte> <bytes>\n";
        return 2;
    }
    const auto count = siftwing::bootstrap::count_probe_byte(argv[2], argv[1][0]);
    std::cout << count << '\n';
    std::cout.flush();
    return std::cout ? 0 : 1;
}
