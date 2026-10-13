/*
 * PROJECT : SIFTWING
 * FILE    : frame_codec_properties.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 仅供测试的有界codec性质入口
 */
#pragma once

#include <cstddef>
#include <string_view>

namespace siftwing::testing {
/** @brief 单次测试输入字节上限，超过时入口立即返回，不是协议Body上限。 */
inline constexpr std::size_t codec_fuzz_max_bytes = 65536;
/**
 * @brief 对任意字节执行有界codec性质检查，违例打印原因后abort供fuzzer保存输入
 *
 * @param[in] bytes
 *     调用期间稳定借用，无私有fixture；不超过codec_fuzz_max_bytes时执行检查。
 *
 * @note 分别按两角色/两本地上限拆帧，各流最多64帧；另将字节作为完整Body构造受信Frame。
 *     所有Buffer/Frame/输出局部拥有。正常异常不吞掉，无socket/线程或可变全局状态。
 *     该入口不提供完整独立语法oracle；独立黄金与头参考由另一CTest负责。
 */
void exercise_codec(std::string_view bytes);
} // namespace siftwing::testing
