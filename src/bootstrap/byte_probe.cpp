/*
 * PROJECT : SIFTWING
 * FILE    : byte_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-06
 * BRIEF   : 实现构建探针的精确字节计数
 * IMPLEMENTATION : 单次线性扫描只比较 char 值，不建立文本或编码状态
 */

#include "siftwing/bootstrap/byte_probe.h"

namespace siftwing::bootstrap {

std::size_t count_probe_byte(std::string_view bytes, char needle) noexcept {
    std::size_t count = 0;
    for (char byte : bytes) {
        if (byte == needle) {
            ++count;
        }
    }
    return count;
}

}  // namespace siftwing::bootstrap
