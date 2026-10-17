/*
 * PROJECT : SIFTWING
 * FILE    : record_format_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 验证长度、损坏、字节拥有性及输出流短写/异常合同，检查不依赖 assert
 */
#include "siftwing/document/record_format.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>

namespace {
using namespace siftwing::document;
unsigned checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
const RecordFormatLimits broad{4096, 32768};
DocumentRecord fixture() {
    auto id = siftwing::base::DocumentId::from_integer(std::numeric_limits<std::uint64_t>::max());
    return {id.value(),
        "标题",
        std::string{"a\0b", 3},
        {SourceKind::rss,
            {"news/a.xml", std::numeric_limits<std::uint64_t>::max()},
            std::string{},
            std::nullopt,
            "raw date"}};
}
// xsputn 明确只接受前缀，证明 ostream::write 的失败会留下不可续写的记录碎片。
class ShortBuffer final : public std::streambuf {
public:
    explicit ShortBuffer(std::size_t capacity) : capacity_(capacity) {}
    std::string bytes;

protected:
    std::streamsize xsputn(const char *data, std::streamsize count) override {
        const auto available = capacity_ - bytes.size();
        const auto wanted = static_cast<std::size_t>(count);
        const auto accepted = std::min(available, wanted);
        bytes.append(data, accepted);
        return static_cast<std::streamsize>(accepted);
    }

private:
    std::size_t capacity_;
};
class ThrowBuffer final : public std::streambuf {
    std::streamsize xsputn(const char *, std::streamsize) override {
        throw std::runtime_error("injected streambuf failure");
    }
};
} // namespace

int main() {
    try {
        auto record = fixture();
        auto encoded = encode_record(record, broad);
        check(static_cast<bool>(encoded), "owned encoding succeeds");
        if (!encoded) {
            return 1;
        }
        const std::string bytes = encoded.value();
        const auto info = validate_record(bytes, broad);
        check(
            info && info.value().doc_id == record.doc_id && info.value().byte_size == bytes.size(),
            "valid complete identity and byte count");
        record.content = "mutated";
        check(encoded.value() == bytes, "encoding owns bytes independently of input");
        // 每个截断点及每一位翻转均应被结构或 CRC 拒绝，而非仅测试魔数。
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            check(!validate_record(std::string_view{bytes}.substr(0, size), broad),
                "every truncation rejected");
        }
        for (std::size_t pos = 0; pos < bytes.size(); ++pos) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                std::string broken = bytes;
                reinterpret_cast<unsigned char *>(broken.data())[pos] ^=
                    static_cast<unsigned char>(1U << bit);
                check(!validate_record(broken, broad), "every single-bit corruption rejected");
            }
        }
        check(!validate_record(bytes + "x", broad), "trailing byte rejected");
        check(!validate_record(bytes + bytes, broad), "multiple records are not one record");
        const auto zero = siftwing::base::DocumentId::from_integer(0);
        DocumentRecord empty{zero.value(), {}, {}, {}};
        auto minimal = encode_record(empty, {1, 88});
        check(minimal && minimal.value().size() == 88 && validate_record(minimal.value(), {1, 88}),
            "all empty fields and zero identity at exact framing budget");
        check(!encode_record(empty, {1, 87}), "framing itself counts toward record budget");
        check(!encode_record(empty, {0, 88}) && !encode_record(empty, {1, 0}),
            "zero budgets rejected");
        check(!validate_record(bytes, {0, 32768}) && !validate_record(bytes, {4096, 0}),
            "validator budgets positive");
        auto original = fixture();
        const RecordFormatLimits exact{10, static_cast<std::uint64_t>(bytes.size())};
        check(static_cast<bool>(encode_record(original, exact)),
            "exact individual and total budgets inclusive");
        check(!encode_record(original, {9, exact.max_record_bytes}), "field exceeds budget by one");
        check(!encode_record(original, {10, exact.max_record_bytes - 1}),
            "total exceeds budget by one");
        check(!validate_record(bytes, {9, exact.max_record_bytes})
                  && !validate_record(bytes, {10, exact.max_record_bytes - 1}),
            "validator enforces both budgets");
        original.source.kind = static_cast<SourceKind>(99);
        check(!encode_record(original, broad),
            "unknown enum rejected instead of serializing underlying number");
        original = fixture();
        for (const auto kind : {SourceKind::txt, SourceKind::rss, SourceKind::jsonl}) {
            original.source.kind = kind;
            auto value = encode_record(original, broad);
            check(value && validate_record(value.value(), broad), "three stable source mappings");
        }
        std::ostringstream output(std::ios::out | std::ios::binary);
        auto written = write_record(output, fixture(), broad);
        check(written && output.str() == bytes && written.value().byte_size == bytes.size(),
            "complete binary stream write");
        check(write_record(output, fixture(), broad) && output.str() == bytes + bytes,
            "caller order concatenation");
        const auto previous = output.str();
        check(!write_record(output, fixture(), {1, 1}) && output.str() == previous && output.good(),
            "encoding error never touches output or its state");
        output.setstate(std::ios::badbit);
        auto not_ready = write_record(output, fixture(), broad);
        check(!not_ready && not_ready.error().context == "record.write" && output.str() == previous,
            "prefailed output rejected");
        for (const auto capacity :
            {std::size_t{0}, std::size_t{1}, bytes.size() - 1, bytes.size()}) {
            for (const bool throws : {false, true}) {
                ShortBuffer buffer{capacity};
                std::ostream stream{&buffer};
                if (throws) {
                    stream.exceptions(std::ios::badbit | std::ios::failbit);
                }
                const auto mask = stream.exceptions();
                auto result = write_record(stream, fixture(), broad);
                check(static_cast<bool>(result) == (capacity == bytes.size()),
                    "short write Result with either exception mask");
                check(buffer.bytes == bytes.substr(0, capacity),
                    "short write prefix honestly retained");
                check(stream.exceptions() == mask, "caller exception mask unchanged");
                if (!result) {
                    check(result.error().context == "record.write", "write failure context");
                }
            }
        }
        ThrowBuffer broken;
        std::ostream stream{&broken};
        check(!write_record(stream, fixture(), broad),
            "default ostream translates streambuf exception to bad state");
        stream.clear();
        stream.exceptions(std::ios::badbit);
        bool propagated = false;
        try {
            (void)write_record(stream, fixture(), broad);
        } catch (const std::runtime_error &) {
            propagated = true;
        }
        check(propagated, "non-ios exception allowed to escape under caller mask");
    } catch (const std::exception &e) {
        ++failures;
        std::cerr << "unexpected exception: " << e.what() << '\n';
    }
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
