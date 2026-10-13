#include "pychron/core/sha1.hpp"

#include <cstddef>
#include <cstring>
#include <string>

namespace pychron {
namespace {

constexpr std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void compress(std::array<std::uint32_t, 5>& h, const std::uint8_t* p) {
  std::uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24) | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16) | (static_cast<std::uint32_t>(p[4 * i + 2]) << 8) |
           static_cast<std::uint32_t>(p[4 * i + 3]);
  for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  auto [a, b, c, d, e] = h;
  for (int i = 0; i < 80; ++i) {
    std::uint32_t f = 0, k = 0;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5a827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdc;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6;
    }
    const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

}  // namespace

std::array<std::uint8_t, 20> sha1(std::string_view bytes) {
  std::array<std::uint32_t, 5> h{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the text's bytes
  const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
  std::size_t n = bytes.size();
  const std::uint64_t bits = std::uint64_t{n} * 8;
  for (; n >= 64; p += 64, n -= 64) compress(h, p);

  std::uint8_t tail[128] = {};
  if (n > 0) std::memcpy(tail, p, n);
  tail[n] = 0x80;
  const std::size_t tail_len = n < 56 ? 64 : 128;
  for (std::size_t i = 0; i < 8; ++i) tail[tail_len - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
  for (std::size_t o = 0; o < tail_len; o += 64) compress(h, tail + o);

  std::array<std::uint8_t, 20> out{};
  for (std::size_t i = 0; i < 5; ++i)
    for (std::size_t b = 0; b < 4; ++b) out[4 * i + b] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * b));
  return out;
}

}  // namespace pychron
