/*
 * PROJECT : SIFTWING
 * FILE    : buffer_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 用独立字节队列验证 Buffer 顺序、容量边界、自追加及分配失败合同
 */

#include "siftwing/net/buffer.hpp"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {
bool fail_next_allocation = false;
std::size_t checks = 0;
std::size_t failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
}

// 仅此单线程测试进程替换普通分配，定点证明真实坏分配路径，不依赖机器耗尽内存。
void *operator new(std::size_t bytes) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc{};
    }
    if (void *ptr = std::malloc(bytes == 0 ? 1 : bytes)) {
        return ptr;
    }
    throw std::bad_alloc{};
}
void *operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}
void operator delete(void *ptr) noexcept {
    std::free(ptr);
}
void operator delete[](void *ptr) noexcept {
    std::free(ptr);
}
void operator delete(void *ptr, std::size_t) noexcept {
    std::free(ptr);
}
void operator delete[](void *ptr, std::size_t) noexcept {
    std::free(ptr);
}

int main() {
    using siftwing::net::Buffer;
    static_assert(!std::is_copy_constructible_v<Buffer> && !std::is_move_constructible_v<Buffer>);
    static_assert(noexcept(std::declval<Buffer &>().consume(0)));
    Buffer zero{0};
    check(zero.append({}) && zero.consume(0) && zero.capacity() == 0, "zero limit empty");
    check(!zero.append("a") && !zero.consume(1), "zero limit rejects nonempty");
    bool invalid = false;
    try {
        Buffer bad{3, 4};
    } catch (const std::invalid_argument &) {
        invalid = true;
    }
    check(invalid, "constructor rejects oversized initial capacity");

    Buffer b{16, 8};
    const std::string binary{"a\0\xffz", 4};
    check(b.append(binary) && b.readable_view() == binary, "binary bytes preserved");
    const auto view = b.readable_view();
    check(!b.consume(5) && b.readable_view().data() == view.data(), "overconsume unchanged");
    check(!b.consume(std::numeric_limits<std::size_t>::max()), "huge consume rejected");
    check(b.append({}) && b.consume(0) && b.readable_view().data() == view.data(),
        "empty operations");
    check(b.append(b.readable_view()) && b.readable_bytes() == 8, "self append in tail");
    check(b.consume(6) && b.append("123456"), "compact prefix");
    check(b.capacity() == 8 && b.readable_view() == binary.substr(2) + "123456",
        "compact keeps order");
    check(b.append("ABCDEFGH") && b.capacity() == 16, "growth doubles");
    const std::string before{b.readable_view()};
    const auto address = b.readable_view().data();
    check(!b.append("x") && b.readable_view() == before && b.readable_view().data() == address,
        "full budget rejection retains view");
    b.clear();
    check(b.capacity() == 16 && b.readable_bytes() == 0 && b.writable_bytes() == 16,
        "clear retains storage");
    check(b.append("1234567890123456") && b.consume(12), "prepare overlapping compact");
    check(b.append(b.readable_view()) && b.readable_view() == "34563456", "self compact overlap");
    Buffer growth{32, 4};
    check(growth.append("abcd") && growth.append(growth.readable_view().substr(1)), "self growth");
    check(growth.readable_view() == "abcdbcd", "growth keeps aliased source alive");
    check(growth.consume(7) && growth.readable_bytes() == 0
              && growth.writable_bytes() == growth.capacity(),
        "consume all resets indices");
    Buffer odd{13, 4};
    check(odd.append("1234567890123") && odd.capacity() == 13, "growth clamps to odd limit");
    Buffer maximum{std::numeric_limits<std::size_t>::max()};
    check(maximum.append("x") && maximum.capacity() == 1024, "huge budget is lazy");

    // 分配失败分别发生在构造、增长和自追加压缩；快照证明可读内容、容量和地址保持。
    fail_next_allocation = true;
    bool caught = false;
    try {
        Buffer bad{64, 64};
    } catch (const std::bad_alloc &) {
        caught = true;
    }
    check(caught && !fail_next_allocation, "constructor allocation failure propagated");
    Buffer failing{256, 128};
    const std::string payload(100, 'q');
    check(failing.append(payload), "prepare allocation tests");
    for (int mode = 0; mode < 2; ++mode) {
        if (mode == 1) {
            check(failing.consume(50), "prepare compact failure");
        }
        const std::string saved{failing.readable_view()};
        const auto ptr = failing.readable_view().data();
        const auto capacity = failing.capacity();
        fail_next_allocation = true;
        caught = false;
        try {
            (void)failing.append(failing.readable_view());
        } catch (const std::bad_alloc &) {
            caught = true;
        }
        check(caught && !fail_next_allocation, "append allocation failure propagated");
        check(failing.readable_view() == saved && failing.readable_view().data() == ptr
                  && failing.capacity() == capacity,
            "append exception leaves state unchanged");
        // 第二轮需要保持相同原数组，第一轮失败没有提交增长。
    }
    check(failing.append(failing.readable_view()), "retry after failure succeeds");

    // 独立 oracle 只用 deque push_back/pop_front；不复刻生产读写索引或增长算法。
    for (std::size_t limit = 0; limit < 66; ++limit) {
        Buffer actual{limit, limit / 3};
        std::deque<char> reference;
        std::size_t state = 73 + limit;
        for (std::size_t step = 0; step < 400; ++step) {
            state = state * 1664525U + 1013904223U;
            const auto count = (state >> 8U) % 19;
            const auto action = state % 5;
            if (action < 3) {
                std::string input;
                for (std::size_t i = 0; i < count; ++i) {
                    input.push_back(static_cast<char>((state + i) % 128));
                }
                const bool expected = count <= limit - reference.size();
                check(actual.append(input) == expected, "reference append result");
                if (expected) {
                    for (char byte : input) {
                        reference.push_back(byte);
                    }
                }
            } else if (action == 3) {
                const bool expected = count <= reference.size();
                check(actual.consume(count) == expected, "reference consume result");
                if (expected) {
                    for (std::size_t i = 0; i < count; ++i) {
                        reference.pop_front();
                    }
                }
            } else {
                actual.clear();
                reference.clear();
            }
            const std::string expected{reference.begin(), reference.end()};
            check(actual.readable_view() == expected, "reference exact bytes");
            check(actual.readable_bytes() == reference.size() && actual.capacity() <= limit
                      && actual.writable_bytes() <= actual.capacity(),
                "public invariants");
        }
    }
    // 协议最大总帧只作为字节长度边界，Buffer 不读取或验证帧头。
    Buffer frame{1048600};
    const std::string large(1048600, 'f');
    check(frame.append(large) && frame.capacity() == 1048600 && !frame.append("x"),
        "max frame budget");
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
