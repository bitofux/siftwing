/*
 * PROJECT : SIFTWING
 * FILE    : frame_codec_fuzzer.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : LLVM libFuzzer原始字节入口，调用共享有界性质检查
 */
#include "frame_codec_properties.hpp"
#include <cstdint>

// libFuzzer借用data，仅在本次调用内存活；不保存指针，不提供自有main或网络动作。
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size <= siftwing::testing::codec_fuzz_max_bytes) {
        siftwing::testing::exercise_codec({reinterpret_cast<const char *>(data), size});
    }
    return 0;
}
