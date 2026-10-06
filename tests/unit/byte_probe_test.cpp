/*
 * PROJECT : SIFTWING
 * FILE    : byte_probe_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-06
 * BRIEF   : 验证构建探针对空输入、重复字节、NUL 和高位字节的精确计数
 */

#include "siftwing/bootstrap/byte_probe.h"

#include <iostream>
#include <string_view>

int main() {
    int failures = 0;
    const auto check = [&failures](std::string_view label, std::string_view bytes,
                                   char needle, std::size_t expected) {
        const auto actual = siftwing::bootstrap::count_probe_byte(bytes, needle);
        if (actual != expected) {
            std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
            ++failures;
        }
    };

    // 基础集合覆盖空、部分匹配、无匹配、全匹配和单字节边界。
    check("empty", {}, 'a', 0);
    check("mixed", "banana", 'a', 3);
    check("absent", "banana", 'z', 0);
    check("all", "aaaa", 'a', 4);
    check("one", "a", 'a', 1);

    // 显式长度视图证明扫描不以 NUL 作为终止符，并保持其后的字节可见。
    const char with_nul[] = {'a', '\0', 'a', '\0'};
    check("embedded NUL", {with_nul, sizeof(with_nul)}, '\0', 2);
    check("after NUL", {with_nul, sizeof(with_nul)}, 'a', 2);

    // char 是否有符号由平台决定；原样保存并比较同一高位字节可避免文本解释。
    const char high_byte = static_cast<char>(0xff);
    const char raw[] = {high_byte, 'a', high_byte};
    check("high byte", {raw, sizeof(raw)}, high_byte, 2);
    return failures == 0 ? 0 : 1;
}
