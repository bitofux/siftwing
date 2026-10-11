/*
 * PROJECT : SIFTWING
 * FILE    : posting.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 定义原始词到文档关系的正整数TF及有序列表校验合同
 */
#pragma once

#include "siftwing/base/id.h"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace siftwing::index {
/**
 * @brief 某一个词在某一个搜索文档中的原始出现次数
 *
 * @note 拥有DocumentId与正uint64次数；词身份由外层词典/列表的调用方关联，不存TermId、
 *     DF、文档总词数、浮点权重或词位。不证明文档存在、去重保留或属于某个快照。
 * @note 复制/移动保留源值；赋值替换完整有效值，自赋值合法。无资源或借用；同一对象的
 *     并发读写由调用方同步。构造不能验证不可从两个整数证明的原文/分词历史。
 */
class Posting final {
public:
    /** @brief 禁止默认构造，缺词关系用列表中无此项表达，不用零TF占位。 */
    Posting() = delete;
    /** @brief 复制完整关系，不借用原对象。 */
    constexpr Posting(const Posting&) noexcept = default;
    /** @brief 移动整数值仍保留源关系，无清空或资源转交。 */
    constexpr Posting(Posting&&) noexcept = default;
    /** @brief 以完整有效关系替换目标，自复制赋值合法。 */
    constexpr Posting& operator=(const Posting&) noexcept = default;
    /** @brief 以完整有效关系替换目标，源值保留、自移动赋值合法。 */
    constexpr Posting& operator=(Posting&&) noexcept = default;

    /**
     * @brief 无损转换正次数后创建原始Posting，不提前转换源整数为无符号数
     *
     * @tparam From
     *     无cv修饰、非bool标准整数类型；按值推导移除实参顶层cv。浮点、枚举、强ID拒绝。
     * @param[in] document
     *     显式DocumentId值；0与UINT64_MAX合法，不验证其存在或快照归属。
     * @param[in] count
     *     词在该文档中的实际出现次数，范围1—UINT64_MAX；不是相对TF、DF或推荐累计。
     *
     * @return
     *     成功拥有完整关系；零、负值或不可无损表示值失败，context固定posting.tf，
     *     失败无部分关系。调用前已发生的截断/回绕不能追回；不设置语料业务预算。
     *
     * @throws
     *     失败诊断分配及未知异常向上传递；普通次数错误使用Result，不承诺noexcept。
     */
    template <typename From>
    [[nodiscard]] static base::Result<Posting> from_count(base::DocumentId document, From count) {
        static_assert(std::is_integral_v<From> && !std::is_same_v<From, bool> &&
                          !std::is_const_v<From> && !std::is_volatile_v<From>,
                      "posting count requires non-cv, non-bool integral types");
        auto converted = base::checked_narrow<std::uint64_t>(count, "posting.tf");
        if (!converted) {
            return base::Result<Posting>::failure(std::move(converted).error());
        }
        if (converted.value() == 0) {
            return base::Result<Posting>::failure({"term frequency must be positive", "posting.tf"});
        }
        return base::Result<Posting>::success(Posting{document, converted.value()});
    }

    /** @brief 按值取得文档身份；不借用、不查库、不证明身份存在。 */
    [[nodiscard]] constexpr base::DocumentId document_id() const noexcept { return document_id_; }
    /** @brief 按值取得精确原始正次数，无浮点转换或归一化。 */
    [[nodiscard]] constexpr std::uint64_t term_frequency() const noexcept { return term_frequency_; }
    /** @brief 比较完整关系值；相同DocId但TF不同不相等，不定义关系大小次序。 */
    friend constexpr bool operator==(Posting lhs, Posting rhs) noexcept {
        return lhs.document_id_ == rhs.document_id_ && lhs.term_frequency_ == rhs.term_frequency_;
    }
    /** @brief 比较完整关系值是否不同。 */
    friend constexpr bool operator!=(Posting lhs, Posting rhs) noexcept { return !(lhs == rhs); }

private:
    constexpr Posting(base::DocumentId document, std::uint64_t count) noexcept
        : document_id_(document), term_frequency_(count) {}
    base::DocumentId document_id_; ///< 拥有的局部文档身份，由上游显式提供，无哨兵。
    std::uint64_t term_frequency_; ///< 拥有的正次数，工厂受检；赋值总是替换完整关系。
};

/** @brief 同一个词的拥有型可变列表；别名自身不保证有序、唯一或快照/词归属。 */
using PostingList = std::vector<Posting>;

/** @brief 列表条数正预算，等于上限合法；不计容器容量/装载成本，非进程硬资源限制。 */
struct PostingListLimits final {
    std::uint64_t max_postings = 0; ///< 0表示未配置；空列表也要求正配置。
};

/**
 * @brief 校验一个词的列表按DocumentId严格递增且在条数预算内，不排序或合并
 *
 * @param[in] postings
 *     调用期间同步借用稳定列表；每项由Posting工厂确保正TF。空列表合法，重复身份即使
 *     TF不同仍拒绝。不修改输入，不持有引用，不生成DocId、TermId、DF或文档统计。
 * @param[in] limits
 *     同步借用正配置；先配置、条数，再严格次序。调用方串行保护列表写入与销毁。
 *
 * @return
 *     success<void>只证明调用时列表的局部结构，不拥有或冻结列表；后续任何修改应重新
 *     校验。failure无部分统计，context posting.limits/size/order；order含首处不递增的
 *     0基元素位置。不验证同词、原文、保留集合、快照归属或TF不超过文档总词数。
 *
 * @throws
 *     失败诊断分配或未知异常向上传递。无文件IO或共享可变状态；未实测并发。
 */
[[nodiscard]] base::Result<void> validate_posting_list(const PostingList& postings,
                                                     const PostingListLimits& limits);
} // namespace siftwing::index
