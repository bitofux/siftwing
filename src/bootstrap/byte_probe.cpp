#include "siftwing/bootstrap/byte_probe.h"

namespace siftwing::bootstrap {

std::size_t count_probe_byte(std::string_view bytes, char needle) noexcept {
    std::size_t count = 0;
    for (char byte : bytes) {
        if (byte == needle) {
            ++count;
        }
    }
    return count;
}

}  // namespace siftwing::bootstrap
