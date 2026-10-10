/*
 * PROJECT : SIFTWING
 * FILE    : cppjieba_tokenizer_test.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 手写词边界、初始化失败、所有权和预算合同的Release有效验收
 */
#include "../support/cppjieba_resources.h"
#include <iostream>
#include <sys/stat.h>
namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
} // namespace
int main() {
    using namespace siftwing::text;
    try {
        siftwing::test::Resources files;
        auto make = CppJiebaTokenizer::create(files.config());
        check(static_cast<bool>(make), "valid resources");
        if (!make) {
            return 1;
        }
        const Tokenizer &tokenizer = *make.value();
        const TokenizationLimits generous{10000, 1000, 1000, 10000};
        auto words = [&](std::string_view input, TokenSequence expected) {
            auto r = tokenizer.tokenize(input, generous);
            check(r && r.value() == expected, "hand-written boundary");
        };
        words("", {});
        words("，😀é\t\n", {});
        words("中文A1 中国,中文", {"中文", "A1", "中国", "中文"});
        words("a_B-C.d'e 12.5", {"a", "B", "C", "d", "e", "12", "5"});
        words("中文\xEF\xBB\xBF中国", {"中文", "中国"});
        words("〇𠀀", {"〇", "𠀀"});
        words("曙光", {"曙", "光"});
        auto hmm = CppJiebaTokenizer::create(files.config(true));
        auto h = hmm.value()->tokenize("曙光", generous);
        check(h && h.value() == TokenSequence{"曙光"}, "HMM B-E independent optimum");
        auto user_config = files.config();
        user_config.user_dictionary_path = (files.root / "user").string();
        auto user = CppJiebaTokenizer::create(user_config);
        auto u = user.value()->tokenize("中国北京", generous);
        check(u && u.value() == TokenSequence{"中国北京"}, "user dictionary changes boundary");
        words("中国北京", {"中国", "北京"});
        // 四项上限分别等于与少一，失败Result访问value必须抛出，不发布部分序列。
        const std::string input = "中文 A1中文";
        const TokenizationLimits exact{input.size(), 3, 6, 14};
        auto exact_result = tokenizer.tokenize(input, exact);
        check(exact_result && exact_result.value() == TokenSequence{"中文", "A1", "中文"},
              "all equal limits");
        const char *contexts[] = {"tokenizer.input_bytes", "tokenizer.tokens",
                                  "tokenizer.token_bytes", "tokenizer.output_bytes"};
        for (unsigned i = 0; i < 4; ++i) {
            auto lim = exact;
            if (i == 0)
                --lim.max_input_bytes;
            if (i == 1)
                --lim.max_tokens;
            if (i == 2)
                --lim.max_token_bytes;
            if (i == 3)
                --lim.max_output_bytes;
            auto r = tokenizer.tokenize(input, lim);
            check(!r && r.error().context == contexts[i], "one less limit");
            bool throws = false;
            try {
                static_cast<void>(r.value());
            } catch (const std::bad_variant_access &) {
                throws = true;
            }
            check(throws, "no failure payload");
            lim = exact;
            if (i == 0)
                lim.max_input_bytes = 0;
            if (i == 1)
                lim.max_tokens = 0;
            if (i == 2)
                lim.max_token_bytes = 0;
            if (i == 3)
                lim.max_output_bytes = 0;
            auto z = tokenizer.tokenize("", lim);
            check(!z && z.error().context == "tokenizer.limits", "zero configuration");
        }
        auto bad = tokenizer.tokenize(std::string("A B\xff", 4), {10, 1, 1, 1});
        check(!bad && bad.error().context == "tokenizer.utf8",
              "complete encoding before output budgets");
        auto nul = tokenizer.tokenize(std::string("A\0B", 3), generous);
        check(!nul && nul.error().context == "tokenizer.nul", "embedded NUL");
        auto invalid = [&](CppJiebaConfig cfg, const char *context) {
            auto r = CppJiebaTokenizer::create(std::move(cfg));
            check(!r && r.error().context == context, "factory diagnostic");
        };
        auto cfg = files.config();
        cfg.max_file_bytes = 0;
        invalid(cfg, "cppjieba.config");
        for (const std::string &path : std::vector<std::string>{
                 "", "dict", "/tmp/../dict", "/tmp//dict", "/tmp/./dict", "/tmp/"}) {
            cfg = files.config();
            cfg.dictionary_path = path;
            invalid(cfg, "cppjieba.config");
        }
        cfg = files.config();
        cfg.dictionary_path = std::string("/a\0b", 4);
        invalid(cfg, "cppjieba.config");
        cfg = files.config();
        cfg.user_dictionary_path = "";
        invalid(cfg, "cppjieba.config");
        cfg = files.config();
        cfg.dictionary_path = (files.root / "missing").string();
        invalid(cfg, "cppjieba.io");
        cfg = files.config();
        cfg.dictionary_path = files.root.string();
        invalid(cfg, "cppjieba.io");
        std::filesystem::create_symlink(files.root / "dict", files.root / "link");
        cfg.dictionary_path = (files.root / "link").string();
        invalid(cfg, "cppjieba.io");
        std::filesystem::create_directory_symlink(files.root, files.root / "dirlink");
        cfg.dictionary_path = (files.root / "dirlink" / "dict").string();
        invalid(cfg, "cppjieba.io");
        check(::mkfifo((files.root / "fifo").c_str(), 0600) == 0, "create nonregular fixture");
        cfg.dictionary_path = (files.root / "fifo").string();
        invalid(cfg, "cppjieba.io");
        cfg = files.config();
        cfg.max_file_bytes = siftwing::test::model.size();
        auto equal = CppJiebaTokenizer::create(cfg);
        check(static_cast<bool>(equal), "equal file budget");
        --cfg.max_file_bytes;
        invalid(cfg, "cppjieba.file_bytes");
        for (const std::string &broken : std::vector<std::string>{
                 "", "中文 0 n\n", "中文 -1 n\n", "中文 2147483648 n\n", "中文 1.5 n\n", "中文 1\n",
                 "中文 1 n extra\n", "中文 1 n\n中文 2 n\n", "中\t文 1 n\n", "中文 1 n\n\n"}) {
            files.write("dict", broken);
            invalid(files.config(), "cppjieba.dictionary");
        }
        files.write("dict", std::string(513, 'x') + " 1 n\n");
        invalid(files.config(), "cppjieba.dictionary");
        files.write("dict", std::string("\xff 1 n", 5));
        invalid(files.config(), "cppjieba.utf8");
        files.write("dict", std::string("中\0 1 n", 8));
        invalid(files.config(), "cppjieba.nul");
        files.write("dict", siftwing::test::dictionary);
        for (const std::string &broken :
             std::vector<std::string>{"bad", "0 0 0 0\n", siftwing::test::model + "0\n"}) {
            files.write("hmm", broken);
            invalid(files.config(), "cppjieba.hmm_model");
        }
        for (const std::string &replacement :
             std::vector<std::string>{"nan", "inf", "1", "-3.15e100", "bad"}) {
            auto m = siftwing::test::model;
            m.replace(m.find("0 -100"), 1, replacement);
            files.write("hmm", m);
            invalid(files.config(), "cppjieba.hmm_model");
        }
        for (const std::string &replacement : std::vector<std::string>{
                 "曙:0,曙:0", "曙光:0", "曙:0,", ",曙:0", "曙:0,,光:0", "曙::0"}) {
            auto m = siftwing::test::model;
            m.replace(m.find("曙:0,光:0"), std::string("曙:0,光:0").size(), replacement);
            files.write("hmm", m);
            invalid(files.config(), "cppjieba.hmm_model");
        }
        files.write("hmm", siftwing::test::model);
        for (const std::string &broken :
             std::vector<std::string>{"中文 0 n\n", "中文 1 n extra\n", "中文\n中文\n"}) {
            files.write("user", broken);
            invalid(user_config, "cppjieba.user_dictionary");
        }
        for (const std::string &valid :
             std::vector<std::string>{"", "中国北京\n", "中国北京 nz\n", "中国北京 1000 nz\r\n"}) {
            files.write("user", valid);
            auto r = CppJiebaTokenizer::create(user_config);
            check(static_cast<bool>(r), "valid user forms");
        }
        // 同路径新实例必须读到新资源；已有实例与输出不借用文件、配置或输入。
        files.write("dict", siftwing::test::dictionary + "中国北京 1000 nz\n");
        auto fresh = CppJiebaTokenizer::create(files.config());
        auto f = fresh.value()->tokenize("中国北京", generous);
        check(f && f.value() == TokenSequence{"中国北京"}, "same path has no stale global cache");
        std::string owned_input = "中文";
        auto owned = tokenizer.tokenize(owned_input, generous);
        owned_input = "changed";
        std::filesystem::remove(files.root / "dict");
        std::filesystem::remove(files.root / "hmm");
        words("中文", {"中文"});
        make.value().reset();
        check(owned && owned.value() == TokenSequence{"中文"},
              "output owns bytes after input/tokenizer destruction");
    } catch (const std::exception &e) {
        ++failures;
        std::cerr << "unexpected: " << e.what() << '\n';
    }
    std::cout << "cppjieba checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
