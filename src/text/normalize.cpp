/*
 * PROJECT : SIFTWING
 * FILE    : normalize.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-10
 * BRIEF   : 严格校验后预计算最终大小，再生成确定性的拥有型规范化文字
 * IMPLEMENTATION :
 * -- utfcpp先验证完整字节，输出预算不能掩盖尾部非法编码
 * -- 空白延迟到下一个实字符才交付，首尾/连续空白不造成临时容量误拒绝
 * -- 同一标量遍历分别供计数与填充使用，保留非ASCII标量的原始UTF-8字节
 */

#include "siftwing/text/normalize.h"
#include "siftwing/base/checked.h"

#include <utf8.h>

#include <utility>

namespace siftwing::text {
namespace {

bool white_space(std::uint32_t point) noexcept {
    // 固定Unicode15.1 White_Space属性，不使用locale或将零宽格式字符猜作空白。
    return (point >= 9U && point <= 13U) || point == 0x20U || point == 0x85U || point == 0xa0U ||
           point == 0x1680U || (point >= 0x2000U && point <= 0x200aU) || point == 0x2028U ||
           point == 0x2029U || point == 0x202fU || point == 0x205fU || point == 0x3000U;
}

// input已严格验证且无NUL；consume同步消费临时小写字符或输入借用段，不保存视图。
// 仅在已有非空文字之后记录pending；末尾pending丢弃，避免全空白与尾部空白被预算误拒绝。
template <typename Consume>
bool visit_normalized(std::string_view input, Consume consume) {
    auto current = input.begin();
    bool started = false;
    bool pending = false;
    while (current != input.end()) {
        const auto start = current;
        const auto point = utf8::next(current, input.end());
        if (white_space(point)) { pending = started; continue; }
        if (pending && !consume(std::string_view{" "})) { return false; }
        pending = false;
        if (point >= 'A' && point <= 'Z') {
            const char lower = static_cast<char>(point + ('a' - 'A'));
            if (!consume(std::string_view{&lower, 1})) { return false; }
        } else {
            if (!consume(std::string_view{start, static_cast<std::size_t>(current - start)})) { return false; }
        }
        started = true;
    }
    return true;
}

base::Result<std::string> failure(std::string message, std::string context) {
    return base::Result<std::string>::failure({std::move(message), std::move(context)});
}

} // namespace

base::Result<std::string> normalize_utf8(std::string_view input, const NormalizationLimits& limits) {
    if (limits.max_input_bytes == 0 || limits.max_output_bytes == 0) {
        return failure("normalization requires positive byte limits", "normalize.limits");
    }
    const auto input_size = base::checked_narrow<std::uint64_t>(input.size(), "normalize.input_bytes");
    if (!input_size || input_size.value() > limits.max_input_bytes) {
        return failure("normalization input exceeds byte limit", "normalize.input_bytes");
    }
    const auto invalid = utf8::find_invalid(input.begin(), input.end());
    if (invalid != input.end()) {
        return failure("invalid UTF-8 sequence at byte " + std::to_string(invalid - input.begin()), "normalize.utf8");
    }
    const auto nul = input.find('\0');
    if (nul != input.npos) { return failure("NUL at byte " + std::to_string(nul), "normalize.nul"); }
    std::uint64_t size = 0;
    const bool fits = visit_normalized(input, [&](std::string_view part) {
        // 每段最多四字节；size始终<=预算，先作减法比较，避免加法溢出。
        if (part.size() > limits.max_output_bytes - size) { return false; }
        size += part.size();
        return true;
    });
    if (!fits) { return failure("normalized text exceeds output byte limit", "normalize.output_bytes"); }
    const auto reserve_size = base::checked_narrow<std::size_t>(size, "normalize.output_bytes");
    if (!reserve_size) { return failure("normalized size cannot be represented", "normalize.output_bytes"); }
    std::string output;
    output.reserve(reserve_size.value());
    static_cast<void>(visit_normalized(input, [&](std::string_view part) { output.append(part); return true; }));
    return base::Result<std::string>::success(std::move(output));
}

} // namespace siftwing::text
