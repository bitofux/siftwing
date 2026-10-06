/*
 * PROJECT : SIFTWING
 * FILE    : byte_probe.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-06
 * BRIEF   : 此模块负责：
 * -- 为构建基线提供同步、精确的单字节计数接口
 * -- 不提供文本、编码、分词或搜索语义
 */

#pragma once

#include <cstddef>
#include <string_view>

namespace siftwing::bootstrap {

/**
 * @brief 统计给定视图中与 needle 完全相等的字节数量
 *
 * @param[in] bytes
 *     调用期间借用的连续字节视图，可为空并可包含 NUL；底层存储须在调用结束前有效且不得
 *     被并发修改。函数不保存或修改该视图。
 * @param[in] needle
 *     按 char 值精确比较的目标字节。
 *
 * @return
 *     匹配字节数，范围为 0..bytes.size()。
 *
 * @note 这是构建探针，不解释字符编码、文本边界或搜索含义。
 * @note 函数不分配资源、不访问共享可变状态；noexcept 承诺覆盖本函数的完整调用路径。
 */
std::size_t count_probe_byte(std::string_view bytes, char needle) noexcept;

}  // namespace siftwing::bootstrap
