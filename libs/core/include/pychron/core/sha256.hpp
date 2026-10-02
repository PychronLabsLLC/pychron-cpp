#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pychron {

using Sha256Digest = std::array<std::uint8_t, 32>;

// Streaming SHA-256 (FIPS 180-4). Feed bytes with update(), then finish()
// once; the hasher must not be reused after finish().
class Sha256 {
 public:
  Sha256();

  void update(std::span<const std::uint8_t> bytes);
  void update(std::string_view bytes);
  Sha256Digest finish();

 private:
  void compress(const std::uint8_t* block);

  std::array<std::uint32_t, 8> h_;
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_ = 0;
};

Sha256Digest sha256(std::string_view bytes);
Sha256Digest sha256(std::span<const std::uint8_t> bytes);

// Lower-case hex of any byte string.
std::string to_hex(std::span<const std::uint8_t> bytes);

}  // namespace pychron
