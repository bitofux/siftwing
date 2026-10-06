/*
 * PROJECT : SIFTWING
 * FILE    : id.h
 * AUTHOR  : bitofux
 * DATE    : 2026-10-07
 * BRIEF   : 此模块负责：
 * -- 区分文档、词项与索引快照的值身份，拒绝跨身份混用
 * -- 保留源整数类型，经受检转换后创建完整 64 位身份值
 * -- 不负责分配编号、查找对象、保证唯一性或保持快照存活
 */

#pragma once

#include "siftwing/base/checked.h"

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace siftwing::base {

namespace detail {

// 标签只参与类型身份，不是运行期字段；不同特化即使载荷相同也不是同一种类型。
struct DocumentIdTag;
struct TermIdTag;
struct SnapshotVersionTag;

/**
 * @brief 三种公开身份共用的数值包装；不同 Tag 产生不同类型
 *
 * @tparam Tag
 *     本模块内部身份标签；公开合同仅覆盖 DocumentId、TermId 与 SnapshotVersion。
 *
 * @note 零和 UINT64_MAX 均合法，无无效哨兵；对象不拥有或借用文档、词项、快照。
 * @note 复制/移动保留源值，赋值替换完整身份；同一对象的并发读写须由调用者同步。
 * @warning 合法值不证明对象存在、编号唯一或快照归属；不能将不同快照的局部编号当成
 *     同一业务对象。读取数值后手动重新包装，也不会增加业务校验。
 */
template <typename Tag>
class StrongId final {
public:
    /** @brief 禁止默认构造，缺失身份须由所属业务模型显式表示。 */
    StrongId() = delete;

    /** @brief 复制同类型身份；源与目标保留相同数值，不复制业务对象。 */
    constexpr StrongId(const StrongId&) noexcept = default;

    /** @brief 移动同类型身份；整数载荷的移动保留源数值，无资源交接。 */
    constexpr StrongId(StrongId&&) noexcept = default;

    /** @brief 以同类型完整值替换当前身份；自赋值合法。 */
    constexpr StrongId& operator=(const StrongId&) noexcept = default;

    /** @brief 以同类型完整值替换当前身份；源数值保留，自移动赋值合法。 */
    constexpr StrongId& operator=(StrongId&&) noexcept = default;

    /**
     * @brief 在源整数可由 uint64_t 无损表示时创建身份
     *
     * @tparam From
     *     无 cv 修饰、非 bool 的标准整数类型；按值推导会移除实参顶层 cv。
     *     浮点、枚举及其他身份类型在实例化时拒绝。
     * @param[in] input
     *     源整数，保留其符号和值域；调用前已发生的转换或截断无法追回。
     * @param[in] context
     *     同步调用期间借用的诊断上下文，默认 "id.from_integer"；可空、可含 NUL。
     *     成功不保存，失败由 checked_narrow 复制为 Error 自有文本。
     *
     * @return
     *     可表示时返回拥有该身份的 success；负数或不可表示值返回 checked_narrow 的
     *     原始 failure，不先转换为无符号整数。
     *
     * @throws
     *     失败诊断构造/分配产生的异常向上传递；普通值域错误用 Result 表达。
     *
     * @note 无状态、无 IO，同一 context 存储在调用期间不得失效或并发修改。
     * @warning 只检查数值表示，不检查文档存在、词项范围、业务容量或版本唯一性。
     */
    template <typename From>
    static Result<StrongId> from_integer(From input,
                                         std::string_view context = "id.from_integer") {
        static_assert(std::is_integral_v<From> && !std::is_same_v<From, bool> &&
                          !std::is_const_v<From> && !std::is_volatile_v<From>,
                      "ID creation requires non-cv, non-bool integral types");
        auto converted = checked_narrow<std::uint64_t>(input, context);
        if (!converted) {
            // 移动拥有型 Error；不再借用 context，也不改写前置模块的失败语义。
            return Result<StrongId>::failure(std::move(converted).error());
        }
        return Result<StrongId>::success(StrongId{converted.value()});
    }

    /**
     * @brief 按值取得底层整数，用于调用方显式处理数值边界
     *
     * @return
     *     完整数值副本，不借用对象存储，也不提供隐式整数转换。
     */
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    /** @brief 按数值判断同类型身份相等，不查找对象或校验快照归属。 */
    friend constexpr bool operator==(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

    /** @brief 按数值判断同类型身份不相等。 */
    friend constexpr bool operator!=(StrongId lhs, StrongId rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    // 仅在工厂完成受检转换后包装；不允许调用者以隐式转换绕过源值域检查。
    explicit constexpr StrongId(std::uint64_t value) noexcept : value_(value) {}

    /** 唯一拥有型载荷；完整 uint64_t 值域，不含无效哨兵或资源引用。 */
    std::uint64_t value_;
};

// 排序仅覆盖文档与词项；替换标签时由 SFINAE 移除候选，不给版本附加时间先后语义。
template <typename Tag>
using OrderedId = std::enable_if_t<std::is_same_v<Tag, DocumentIdTag> ||
                                       std::is_same_v<Tag, TermIdTag>, int>;

/** @brief 判断同类型文档/词项身份的数值小于关系；不接受快照版本。 */
template <typename Tag, OrderedId<Tag> = 0>
constexpr bool operator<(StrongId<Tag> lhs, StrongId<Tag> rhs) noexcept {
    return lhs.value() < rhs.value();
}

/** @brief 判断同类型文档/词项身份的数值大于关系；不接受快照版本。 */
template <typename Tag, OrderedId<Tag> = 0>
constexpr bool operator>(StrongId<Tag> lhs, StrongId<Tag> rhs) noexcept {
    return rhs < lhs;
}

/** @brief 判断同类型文档/词项身份的数值小于等于关系；不接受快照版本。 */
template <typename Tag, OrderedId<Tag> = 0>
constexpr bool operator<=(StrongId<Tag> lhs, StrongId<Tag> rhs) noexcept {
    return !(rhs < lhs);
}

/** @brief 判断同类型文档/词项身份的数值大于等于关系；不接受快照版本。 */
template <typename Tag, OrderedId<Tag> = 0>
constexpr bool operator>=(StrongId<Tag> lhs, StrongId<Tag> rhs) noexcept {
    return !(lhs < rhs);
}

}  // namespace detail

/**
 * @brief 一个索引视图内的文档身份，按数值提供同类型比较
 *
 * @note 别名目标是独立标签特化，与 TermId、SnapshotVersion 是不同类型；不是整数别名。
 *     值域、创建、所有权与错误合同见 StrongId。不保证跨快照编号对应同一文档。
 */
using DocumentId = detail::StrongId<detail::DocumentIdTag>;

/**
 * @brief 一个索引视图内规范化词项的身份，按数值提供同类型比较
 *
 * @note 不拥有 term 字符串，不证明词典包含该值；类型与 DocumentId、SnapshotVersion 不同。
 *     值域、创建、所有权与错误合同见 StrongId。
 */
using TermId = detail::StrongId<detail::TermIdTag>;

/**
 * @brief 一份逻辑索引快照的不透明身份令牌，仅提供同类型相等比较
 *
 * @note 不表示格式版本或发布时间，不持有视图，不负责生成或保证唯一性。
 *     发布者与加载者负责身份一致性；值域、创建、所有权与错误合同见 StrongId。
 */
using SnapshotVersion = detail::StrongId<detail::SnapshotVersionTag>;

}  // namespace siftwing::base
