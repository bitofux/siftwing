/*
 * PROJECT : SIFTWING
 * FILE    : cppjieba_tokenizer.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 隔离cppjieba资源初始化及中文/ASCII混合分词合同
 */

#pragma once

#include "siftwing/text/tokenizer.h"

#include <memory>
#include <optional>

namespace siftwing::text {

/** @brief 固定分段/过滤规则版本；实际词边界还取决于词典、模型、用户词典及HMM开关。 */
inline constexpr std::uint32_t cppjieba_tokenization_policy_version = 1;

/** @brief 初始化时显式提供且按值拥有的资源配置；不从工作目录推断默认资源。 */
struct CppJiebaConfig final {
    std::string dictionary_path; ///< 主词典绝对路径，严格UTF-8，非空的word freq tag行。
    std::string hmm_model_path; ///< HMM绝对路径；无论开关均验证/装载完整模型。
    std::optional<std::string>
        user_dictionary_path; ///< 单一用户词典绝对路径；nullopt不加载，空文件合法。
    std::uint64_t max_file_bytes = 0; ///< 各文件独立正字节预算，等于上限合法，原CRLF也计入。
    bool use_hmm = true; ///< 汉字段是否使用词典与HMM混合；false仅词典DAG路径。
};

/**
 * @brief 初始化后只读的中文分词适配，公共类型不暴露第三方头或词典对象
 *
 * @note 支持汉字段为U+3007及固定CJK统一/兼容汉字块：3400—4DBF、4E00—9FFF、F900—FAFF、
 *     20000—2A6DF、2A700—2B73F、2B740—2B81F、2B820—2CEAF、2CEB0—2EBEF、2EBF0—2EE5F、
 *     2F800—2FA1F、30000—3134F、31350—323AF。这是明确块范围，不承诺完整Unicode Han属性。
 *     最大连续汉字段交给cppjieba；最大连续ASCII[A-Za-z0-9]+沿英文词边界；其余为分隔，
 *     包括非ASCII拉丁、emoji、标点、空白、U+FEFF、组合/零宽码点。顺序/重复/大小写保留，
 *     无音译、词干或隐式规范化/停用词过滤；调用方可以先显式调用normalize_utf8。
 * @note 不提供运行时增删词、IDF/关键词/停用词加载或路径缓存。初始化后资源文件变化不
 *     影响当前对象。对象不可复制/移动；unique_ptr可移动拥有它。同实例分词使用只读资源
 *     和局部状态；外部input/limits并发写仍需同步，线程并发验证另属专项证据。
 */
class CppJiebaTokenizer final : public Tokenizer {
  public:
    /**
     * @brief 受限加载并严格验证资源后一次性发布拥有型适配器
     *
     * @param[in] config
     *     按值取得配置，路径必须绝对且每个组件无符号链接，目标为普通文件；初始化期文件
     *     不被并发写。工厂只读文件，全部读入/验证后使用内存构造补丁，不让库再次读取路径。
     *
     * @return
     *     success拥有非空unique_ptr；failure只拥有诊断、不发布未初始化对象。
     *     context为cppjieba.config/io/file_bytes/utf8/nul/dictionary/user_dictionary/hmm_model。
     *     主词典非空，freq为1—INT_MAX的十进制整数，tag非空；用户行word/word tag/
     *     word freq tag，空文件合法。两词典word最多512码点，拒绝空白/重复词。
     *     HMM为注释/空行之外的9行：4初始概率、4×4转移概率、四状态非空单码点发射表；
     *     概率为有限的[-3.14e100,0]数，发射表无重复，拒绝缺失/多余/坏数值。
     *
     * @note 文件读取检查不提供恶意并发修改的原子快照。库使用的是本次已验证的拥有数据。
     *     文件预算不包含解析后Trie/模型内存；第三方分配失败和未知异常向上传递，
     *     不承诺故障注入/OOM下上游内部构造的强异常保证。
     */
    [[nodiscard]] static base::Result<std::unique_ptr<CppJiebaTokenizer>>
    create(CppJiebaConfig config);

    /** @brief 在实现文件内释放segment，再释放其借用的模型/词典；析构不抛出。 */
    ~CppJiebaTokenizer() noexcept override;
    CppJiebaTokenizer(const CppJiebaTokenizer &) = delete; ///< 不复制词典/模型拥有者。
    CppJiebaTokenizer &operator=(const CppJiebaTokenizer &) = delete; ///< 不替换已初始化资源。
    CppJiebaTokenizer(CppJiebaTokenizer &&) = delete; ///< 保持对象内部资源地址与身份稳定。
    CppJiebaTokenizer &operator=(CppJiebaTokenizer &&) = delete; ///< 通过unique_ptr转移所有权。

    /**
     * @brief 沿Tokenizer严格验证/拥有结果合同分词，运行期间不读取资源文件
     *
     * @param[in] input
     *     同步借用UTF-8，原文不修改；库索引约束使实际输入另限于INT_MAX/4字节。
     * @param[in] limits
     *     四正预算；先配置/输入字节（含库硬索引范围），再完整UTF-8/NUL，再按源token顺序
     *     检查单token字节/条数/累计词字节，最后复制全部拥有词。
     *
     * @return
     *     全部词或诊断，context沿Tokenizer；空/仅分隔符成功为空，无部分序列或截词。
     *
     * @note 库先建立各汉字段的Rune/DAG/HMM临时结构，再实施输出预算；这些不属于进程
     *     内存/CPU硬上限。库返回范围若破坏连续/完整覆盖不变量，作为未知内部异常上传。
     */
    [[nodiscard]] base::Result<TokenSequence>
    tokenize(std::string_view input, const TokenizationLimits &limits) const override;

  private:
    struct Impl;
    explicit CppJiebaTokenizer(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> implementation_;
};

} // namespace siftwing::text
