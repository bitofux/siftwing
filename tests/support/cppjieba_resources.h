/*
 * PROJECT : SIFTWING
 * FILE    : cppjieba_resources.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 原创可手算词典/HMM及独立临时目录，仅供行为测试
 */
#pragma once
#include "siftwing/text/cppjieba_tokenizer.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
namespace siftwing::test {
// 四词共301频次；“中文”优于两个未知单字；未知“曙光”的HMM最优路径为B→E。
inline const std::string dictionary = "北京 100 ns\n中国 100 ns\n中文 100 n\n单 1 n\n";
inline const std::string model =
    "# 原创B/E/M/S模型\n0 -100 -100 -100\n"
    "-100 0 -100 -100\n0 -100 -100 -100\n-100 0 -100 -100\n0 -100 -100 -100\n"
    "曙:0,光:0\n曙:0,光:0\n曙:0,光:0\n曙:-100,光:-100\n";
class Resources final {
  public:
    Resources() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "siftwing-cppjieba-XXXXXX").string();
        char *created = ::mkdtemp(pattern.data());
        if (!created) {
            throw std::runtime_error("mkdtemp failed");
        }
        root = created;
        write("dict", dictionary);
        write("hmm", model);
        write("user", "中国北京 1000 nz\n");
    }
    ~Resources() noexcept {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    Resources(const Resources &) = delete;
    Resources &operator=(const Resources &) = delete;
    void write(const std::string &name, const std::string &bytes) const {
        std::ofstream stream(root / name, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw std::runtime_error("fixture write failed");
        }
    }
    [[nodiscard]] text::CppJiebaConfig config(bool hmm = false) const {
        return {(root / "dict").string(), (root / "hmm").string(), std::nullopt, 100000, hmm};
    }
    std::filesystem::path root;
};
} // namespace siftwing::test
