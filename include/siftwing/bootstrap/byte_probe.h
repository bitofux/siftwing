#pragma once

#include <cstddef>
#include <string_view>

namespace siftwing::bootstrap {

// Build probe only: count exact bytes, without text or encoding semantics.
// Borrows bytes for this call; no input is retained or modified.
std::size_t count_probe_byte(std::string_view bytes, char needle) noexcept;

}  // namespace siftwing::bootstrap
