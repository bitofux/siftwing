/*
 * PROJECT : SIFTWING
 * FILE    : frame_decoder_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 用手工线字节验证分片、粘连、拒绝顺序、拥有性和分配失败保持
 */

#include "siftwing/net/frame_decoder.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
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
using namespace siftwing::net;

// 来自公开线合同的手工完整 PING，不调用生产编码/解码辅助函数构造期望。
std::string ping_bytes() {
    const unsigned char golden[] = {0x53,
        0x46,
        0x54,
        0x57,
        1,
        0x18,
        0,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        2,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        0x7b,
        0x7d};
    return {reinterpret_cast<const char *>(golden), sizeof golden};
}

// 参考构造从低位用除法/余数填字段，与生产逐字节左移读取的方向独立。
void field(std::string &bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = width; i != 0; --i) {
        bytes[offset + i - 1] = static_cast<char>(value % 256);
        value /= 256;
    }
}
std::string wire(
    std::uint16_t type, std::uint16_t status, std::uint64_t id, const std::string &body) {
    auto bytes = ping_bytes();
    bytes.resize(24);
    field(bytes, 6, 2, type);
    field(bytes, 10, 2, status);
    field(bytes, 12, 4, body.size());
    field(bytes, 16, 8, id);
    return bytes + body;
}

void frame_is(const DecodeResult &result,
    MessageType type,
    std::uint16_t status,
    std::uint64_t id,
    const std::string &body) {
    const auto *frame = std::get_if<Frame>(&result);
    check(frame && frame->message_type == type && frame->status == status && frame->request_id == id
              && frame->body == body,
        "decoded fields match independent input");
}

void unchanged(Buffer &input,
    ReceiveRole role,
    DecodeError expected,
    std::size_t limit = frame_max_body_bytes) {
    const std::string original{input.readable_view()};
    const auto address = input.readable_view().data();
    const auto capacity = input.capacity();
    // 非法头路径应不分配；若意外分配会抛异常使测试失败。
    fail_next_allocation = true;
    const auto result = decode_next(input, role, limit);
    const bool no_allocation = fail_next_allocation;
    fail_next_allocation = false;
    const auto *error = std::get_if<DecodeError>(&result);
    check(error && *error == expected, "expected rejection category");
    check(no_allocation && input.readable_view() == original
              && input.readable_view().data() == address && input.capacity() == capacity,
        "rejection preserves bytes address capacity and does not allocate");
}

void rejection(std::string bytes,
    DecodeError expected,
    ReceiveRole role = ReceiveRole::server,
    std::size_t limit = frame_max_body_bytes) {
    Buffer input{bytes.size() + frame_header_bytes + frame_max_body_bytes};
    check(input.append(bytes), "append malformed input");
    unchanged(input, role, expected, limit);
    unchanged(input, role, expected, limit); // 无内部重同步或隐含消费，重复仍得到同一拒绝。
}
} // namespace

// 仅当前单线程测试进程的定点分配失败，不让真实机器耗尽内存。
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
    const auto ping = ping_bytes();
    const std::string body = "{\"q\":\"中文\",\"n\":123}";
    const auto search = wire(3, 0, 0xfedcba9876543210ULL, body);
    // 每个切割点（含空输入和完整头）均检查无消费，再补余下字节完成。
    for (const auto &bytes : {ping, search}) {
        for (std::size_t split = 0; split < bytes.size(); ++split) {
            Buffer input{bytes.size()};
            check(input.append(std::string_view{bytes}.substr(0, split)), "append first fragment");
            const auto view = input.readable_view();
            fail_next_allocation = true;
            const auto result = decode_next(input, ReceiveRole::server);
            const bool no_allocation = fail_next_allocation;
            fail_next_allocation = false;
            check(std::holds_alternative<std::monostate>(result) && no_allocation
                      && input.readable_bytes() == split
                      && input.readable_view().data() == view.data(),
                "every incomplete split preserves input and does not allocate");
            check(input.append(std::string_view{bytes}.substr(split)), "append second fragment");
            const auto ready = decode_next(input, ReceiveRole::server);
            frame_is(ready,
                bytes == ping ? MessageType::ping : MessageType::search_request,
                0,
                bytes == ping ? 0x0102030405060708ULL : 0xfedcba9876543210ULL,
                bytes == ping ? "{}" : body);
            check(input.readable_bytes() == 0, "consume exactly completed frame");
        }
    }
    Buffer single{search.size()};
    for (std::size_t i = 0; i < search.size(); ++i) {
        check(single.append(std::string_view{search}.substr(i, 1)), "append one byte");
        const auto result = decode_next(single, ReceiveRole::server);
        if (i + 1 == search.size()) {
            frame_is(result, MessageType::search_request, 0, 0xfedcba9876543210ULL, body);
        } else {
            check(
                std::holds_alternative<std::monostate>(result) && single.readable_bytes() == i + 1,
                "one byte input never publishes partial frame");
        }
    }
    // 粘连两帧及第三帧的所有前缀；前两次调用不能把尾部半帧误丢弃。
    for (std::size_t tail = 0; tail < search.size(); ++tail) {
        Buffer input{ping.size() + search.size() * 2};
        check(input.append(ping + search + search.substr(0, tail)), "append glued frames");
        const auto first = decode_next(input, ReceiveRole::server);
        frame_is(first, MessageType::ping, 0, 0x0102030405060708ULL, "{}");
        frame_is(decode_next(input, ReceiveRole::server),
            MessageType::search_request,
            0,
            0xfedcba9876543210ULL,
            body);
        check(input.readable_view() == search.substr(0, tail), "glued suffix byte order");
        check(std::holds_alternative<std::monostate>(decode_next(input, ReceiveRole::server)),
            "glued incomplete suffix waits");
        check(input.append(search.substr(tail)), "append glued suffix completion");
        frame_is(decode_next(input, ReceiveRole::server),
            MessageType::search_request,
            0,
            0xfedcba9876543210ULL,
            body);
        input.clear();
        frame_is(first, MessageType::ping, 0, 0x0102030405060708ULL, "{}");
    }
    // 九种编号、两个角色、合法及紧邻非法 status 和高位 request_id；不猜未知编号。
    const std::uint16_t types[] = {1, 2, 3, 4, 0x8001, 0x8002, 0x8003, 0x8004, 0xffff};
    for (const auto type : types) {
        const auto role = type < 0x8000 ? ReceiveRole::server : ReceiveRole::client;
        for (std::uint16_t status = 0; status <= 7; ++status) {
            const auto bytes = wire(type, status, std::numeric_limits<std::uint64_t>::max(), "{}");
            if (type == 0xffff ? (status >= 1 && status <= 6) : status == 0) {
                Buffer input{bytes.size()};
                check(input.append(bytes), "append known type");
                frame_is(decode_next(input, role),
                    static_cast<MessageType>(type),
                    status,
                    std::numeric_limits<std::uint64_t>::max(),
                    "{}");
                rejection(bytes,
                    DecodeError::direction,
                    role == ReceiveRole::server ? ReceiveRole::client : ReceiveRole::server);
            } else {
                rejection(bytes, DecodeError::status, role);
            }
        }
    }
    const std::pair<std::size_t, DecodeError> fields[] = {{0, DecodeError::magic},
        {4, DecodeError::version},
        {5, DecodeError::header_size},
        {7, DecodeError::message_type},
        {9, DecodeError::flags},
        {11, DecodeError::status}};
    for (const auto &entry : fields) {
        auto bytes = ping;
        bytes[entry.first] = static_cast<char>(0x7f);
        rejection(bytes, entry.second);
    }
    for (const std::uint64_t length : {0ULL, 1ULL, 1048577ULL, 0xffffffffULL}) {
        auto bytes = ping.substr(0, 24); // 不需要 Body 即拒绝，尤其不能等约 4 GiB。
        field(bytes, 12, 4, length);
        rejection(bytes, DecodeError::body_length);
    }
    auto zero_id = ping;
    field(zero_id, 16, 8, 0);
    rejection(zero_id, DecodeError::request_id);
    rejection(search.substr(0, 24), DecodeError::body_length, ReceiveRole::server, body.size() - 1);
    // 多处损坏逐一修复，验证公开协议的拒绝优先级。
    auto bad = ping;
    bad[0] = 'x';
    bad[4] = 2;
    bad[5] = 25;
    field(bad, 6, 2, 0x8005);
    field(bad, 8, 2, 1);
    field(bad, 10, 2, 9);
    field(bad, 12, 4, 0xffffffffULL);
    field(bad, 16, 8, 0);
    rejection(bad, DecodeError::magic);
    bad[0] = 'S';
    rejection(bad, DecodeError::version);
    bad[4] = 1;
    rejection(bad, DecodeError::header_size);
    bad[5] = 24;
    rejection(bad, DecodeError::message_type);
    field(bad, 6, 2, 0x8001);
    rejection(bad, DecodeError::flags);
    field(bad, 8, 2, 0);
    rejection(bad, DecodeError::status);
    field(bad, 10, 2, 0);
    rejection(bad, DecodeError::body_length);
    field(bad, 12, 4, 2);
    rejection(bad, DecodeError::request_id);
    field(bad, 16, 8, 1);
    rejection(bad, DecodeError::direction);

    Buffer too_small{24};
    check(too_small.append(ping.substr(0, 24)), "append impossible frame header");
    unchanged(too_small, ReceiveRole::server, DecodeError::buffer_limit);
    Buffer empty{0};
    check(std::holds_alternative<std::monostate>(decode_next(empty, ReceiveRole::server)),
        "empty zero budget waits without reading header");
    for (const std::size_t limit : {std::size_t{0},
             std::size_t{1},
             frame_max_body_bytes + 1,
             std::numeric_limits<std::size_t>::max()}) {
        bool threw = false;
        try {
            (void)decode_next(empty, ReceiveRole::server, limit);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        check(threw && empty.readable_bytes() == 0, "invalid decoder budget throws unchanged");
    }
    bool bad_role = false;
    try {
        (void)decode_next(empty, static_cast<ReceiveRole>(99));
    } catch (const std::invalid_argument &) {
        bad_role = true;
    }
    check(bad_role, "invalid role rejected");

    // 上界完整解码及精确本地预算；非 JSON 字节刻意证明本层仅拆帧。
    const std::string large_body(frame_max_body_bytes, '\xff');
    const auto large = wire(3, 0, std::numeric_limits<std::uint64_t>::max(), large_body);
    Buffer maximum{large.size()};
    check(maximum.append(large), "append maximum complete frame");
    auto owned = decode_next(maximum, ReceiveRole::server);
    frame_is(owned,
        MessageType::search_request,
        0,
        std::numeric_limits<std::uint64_t>::max(),
        large_body);
    maximum.clear();
    check(maximum.append(ping), "reuse input after owning decode");
    frame_is(owned,
        MessageType::search_request,
        0,
        std::numeric_limits<std::uint64_t>::max(),
        large_body);

    Buffer allocation{search.size() * 2};
    check(allocation.append(ping + search), "allocation input contains two frames");
    (void)decode_next(allocation, ReceiveRole::server); // 令 read_ 非零再验证强异常保证。
    const std::string original{allocation.readable_view()};
    const auto address = allocation.readable_view().data();
    const auto capacity = allocation.capacity();
    bool failed = false;
    fail_next_allocation = true;
    try {
        (void)decode_next(allocation, ReceiveRole::server);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    fail_next_allocation = false;
    check(failed && allocation.readable_view() == original
              && allocation.readable_view().data() == address && allocation.capacity() == capacity,
        "body allocation failure preserves unread bytes indices capacity and address");
    frame_is(decode_next(allocation, ReceiveRole::server, body.size()),
        MessageType::search_request,
        0,
        0xfedcba9876543210ULL,
        body);
    check(allocation.readable_bytes() == 0, "retry after bad allocation succeeds");
    const std::string binary{"\0\xff", 2};
    Buffer raw{26};
    check(raw.append(wire(1, 0, 1, binary)), "append opaque ping body");
    frame_is(decode_next(raw, ReceiveRole::server), MessageType::ping, 0, 1, binary);

    // 长 Body 的任意分块与 Buffer 压缩/增长互作，消费结果顺序由输入列表独立给出。
    const auto longer = wire(3, 0, 17, std::string(150, 'q'));
    const std::string stream = ping + longer + ping + search;
    for (std::size_t chunk = 1; chunk <= 47; ++chunk) {
        Buffer input{stream.size(), 32};
        std::size_t count = 0;
        for (std::size_t offset = 0; offset < stream.size(); offset += chunk) {
            check(input.append(std::string_view{stream}.substr(offset, chunk)),
                "append stream partition");
            while (true) {
                const auto result = decode_next(input, ReceiveRole::server);
                if (std::holds_alternative<std::monostate>(result)) {
                    break;
                }
                const auto *frame = std::get_if<Frame>(&result);
                check(frame != nullptr && count < 4, "partition produces only expected frames");
                if (!frame || count >= 4) {
                    break;
                }
                const std::uint64_t expected[] = {
                    0x0102030405060708ULL, 17, 0x0102030405060708ULL, 0xfedcba9876543210ULL};
                check(frame->request_id == expected[count], "partition preserves frame ordering");
                ++count;
            }
        }
        check(count == 4 && input.readable_bytes() == 0, "partition consumes complete stream");
    }
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
