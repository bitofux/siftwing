#include "siftwing/bootstrap/byte_probe.h"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc != 3 || std::string_view{argv[1]}.size() != 1) {
        std::cerr << "usage: siftwing_build_probe <single-byte> <bytes>\n";
        return 2;
    }
    const auto count = siftwing::bootstrap::count_probe_byte(argv[2], argv[1][0]);
    std::cout << count << '\n';
    std::cout.flush();
    return std::cout ? 0 : 1;
}
