/*
 * PROJECT : SIFTWING
 * FILE    : buffer.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-13
 * BRIEF   : 实现连续 Buffer 的预算检查、空间复用及异常前状态提交
 */

#include "siftwing/net/buffer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace siftwing::net {

Buffer::Buffer(std::size_t max_bytes, std::size_t initial_capacity) : max_bytes_(max_bytes) {
    if (initial_capacity > max_bytes_) {
        throw std::invalid_argument("buffer initial capacity exceeds limit");
    }
    if (initial_capacity != 0) {
        storage_.reset(new char[initial_capacity]);
        capacity_ = initial_capacity;
    }
}

std::string_view Buffer::readable_view() const noexcept {
    return readable_bytes() == 0 ? std::string_view{}
                                 : std::string_view{storage_.get() + read_, readable_bytes()};
}

bool Buffer::append(std::string_view bytes) {
    const auto readable = readable_bytes();
    // 减法检查先于加法，避免恶意长度引起 size_t 回绕；超限路径不读取输入。
    if (bytes.size() > max_bytes_ - readable) {
        return false;
    }
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() <= writable_bytes()) {
        std::memmove(storage_.get() + write_, bytes.data(), bytes.size());
        write_ += bytes.size();
        return true;
    }
    const auto required = readable + bytes.size();
    if (required <= capacity_) {
        // 压缩可能覆盖自追加的源；先复制输入，分配失败时原数组和索引尚未改变。
        const std::string incoming{bytes};
        std::memmove(storage_.get(), storage_.get() + read_, readable);
        std::memcpy(storage_.get() + readable, incoming.data(), incoming.size());
    } else {
        // 饱和倍增不计算溢出的乘法。先填新数组，再交接所有权，保留强异常保证。
        auto next = capacity_ == 0 ? std::min(std::size_t{1024}, max_bytes_) : capacity_;
        while (next < required) {
            next = next > max_bytes_ - next ? max_bytes_ : next * 2;
        }
        auto replacement = std::unique_ptr<char[]>{new char[next]};
        if (readable != 0) {
            std::memcpy(replacement.get(), storage_.get() + read_, readable);
        }
        std::memcpy(replacement.get() + readable, bytes.data(), bytes.size());
        storage_.swap(replacement);
        capacity_ = next;
    }
    read_ = 0;
    write_ = required;
    return true;
}

bool Buffer::consume(std::size_t bytes) noexcept {
    if (bytes > readable_bytes()) {
        return false;
    }
    read_ += bytes;
    if (read_ == write_) {
        clear();
    }
    return true;
}

void Buffer::clear() noexcept {
    read_ = 0;
    write_ = 0;
}

} // namespace siftwing::net
