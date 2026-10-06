#include "siftwing/bootstrap/byte_probe.h"

#include <iostream>
#include <string_view>

int main() {
    int failures = 0;
    const auto check = [&failures](std::string_view label, std::string_view bytes,
                                   char needle, std::size_t expected) {
        const auto actual = siftwing::bootstrap::count_probe_byte(bytes, needle);
        if (actual != expected) {
            std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
            ++failures;
        }
    };

    check("empty", {}, 'a', 0);
    check("mixed", "banana", 'a', 3);
    check("absent", "banana", 'z', 0);
    check("all", "aaaa", 'a', 4);
    check("one", "a", 'a', 1);

    const char with_nul[] = {'a', '\0', 'a', '\0'};
    check("embedded NUL", {with_nul, sizeof(with_nul)}, '\0', 2);
    check("after NUL", {with_nul, sizeof(with_nul)}, 'a', 2);

    const char high_byte = static_cast<char>(0xff);
    const char raw[] = {high_byte, 'a', high_byte};
    check("high byte", {raw, sizeof(raw)}, high_byte, 2);
    return failures == 0 ? 0 : 1;
}
