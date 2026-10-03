#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace pychron {

// SHA-1 (FIPS 180-4), one-shot. Not collision resistant: used only where a
// standard demands it (RFC 9562 UUIDv5 name-based ids), never for integrity.
std::array<std::uint8_t, 20> sha1(std::string_view bytes);

}  // namespace pychron
