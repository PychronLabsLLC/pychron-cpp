#include "pychron/core/sha256.hpp"

#include <algorithm>
#include <cstring>

namespace pychron {
namespace {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::compress(const std::uint8_t* p) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) | (std::uint32_t(p[4 * i + 2]) << 8) |
           std::uint32_t(p[4 * i + 3]);
  for (int i = 16; i < 64; ++i) {
    const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  auto a = h_;
  for (int i = 0; i < 64; ++i) {
    const auto s1 = rotr(a[4], 6) ^ rotr(a[4], 11) ^ rotr(a[4], 25);
    const auto ch = (a[4] & a[5]) ^ (~a[4] & a[6]);
    const auto t1 = a[7] + s1 + ch + kK[i] + w[i];
    const auto s0 = rotr(a[0], 2) ^ rotr(a[0], 13) ^ rotr(a[0], 22);
    const auto mj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
    const auto t2 = s0 + mj;
    a[7] = a[6];
    a[6] = a[5];
    a[5] = a[4];
    a[4] = a[3] + t1;
    a[3] = a[2];
    a[2] = a[1];
    a[1] = a[0];
    a[0] = t1 + t2;
  }
  for (int i = 0; i < 8; ++i) h_[i] += a[i];
}

void Sha256::update(std::span<const std::uint8_t> bytes) {
  const std::uint8_t* p = bytes.data();
  std::size_t n = bytes.size();
  total_ += n;
  if (buffered_ > 0) {
    const std::size_t take = std::min(n, buffer_.size() - buffered_);
    std::memcpy(buffer_.data() + buffered_, p, take);
    buffered_ += take;
    p += take;
    n -= take;
    if (buffered_ < buffer_.size()) return;
    compress(buffer_.data());
    buffered_ = 0;
  }
  for (; n >= 64; p += 64, n -= 64) compress(p);
  if (n > 0) {
    std::memcpy(buffer_.data(), p, n);
    buffered_ = n;
  }
}

void Sha256::update(std::string_view bytes) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the text's bytes
  update(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

Sha256Digest Sha256::finish() {
  const std::uint64_t bits = total_ * 8;
  std::uint8_t tail[128] = {};
  std::memcpy(tail, buffer_.data(), buffered_);
  tail[buffered_] = 0x80;
  const std::size_t tail_len = buffered_ < 56 ? 64 : 128;
  for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
  for (std::size_t o = 0; o < tail_len; o += 64) compress(tail + o);

  Sha256Digest out{};
  for (std::size_t i = 0; i < 8; ++i)
    for (std::size_t b = 0; b < 4; ++b) out[4 * i + b] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * b));
  return out;
}

Sha256Digest sha256(std::string_view bytes) {
  Sha256 h;
  h.update(bytes);
  return h.finish();
}

Sha256Digest sha256(std::span<const std::uint8_t> bytes) {
  Sha256 h;
  h.update(bytes);
  return h.finish();
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (auto b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xf]);
  }
  return out;
}

}  // namespace pychron
