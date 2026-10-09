#include "pychron/persistence/blob.hpp"

#include <array>
#include <bit>

namespace pychron::persistence {
namespace {

void put_le(Bytes& out, float f) {
  const auto u = std::bit_cast<std::uint32_t>(f);
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
}

void put_be(Bytes& out, float f) {
  const auto u = std::bit_cast<std::uint32_t>(f);
  for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
}

float get_le(const std::uint8_t* p) {
  std::uint32_t u = 0;
  for (int i = 3; i >= 0; --i) u = u << 8 | p[i];
  return std::bit_cast<float>(u);
}

float get_be(const std::uint8_t* p) {
  std::uint32_t u = 0;
  for (int i = 0; i < 4; ++i) u = u << 8 | p[i];
  return std::bit_cast<float>(u);
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

Bytes encode_tv(std::span<const TvPoint> points) {
  Bytes out;
  out.reserve(points.size() * 8);
  for (const auto& p : points) {
    put_le(out, p.t);
    put_le(out, p.v);
  }
  return out;
}

Result<std::vector<TvPoint>> decode_tv(std::span<const std::uint8_t> bytes) {
  if (bytes.size() % 8 != 0) return fail(ErrorKind::Protocol, "f32le-tv/1: length is not a multiple of 8");
  std::vector<TvPoint> out(bytes.size() / 8);
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = {get_le(&bytes[8 * i]), get_le(&bytes[8 * i + 4])};
  return out;
}

Bytes encode_tvs(std::span<const TvsPoint> points) {
  Bytes out;
  out.reserve(points.size() * 12);
  for (const auto& p : points) {
    put_le(out, p.t);
    put_le(out, p.v);
    put_le(out, p.s);
  }
  return out;
}

Result<std::vector<TvsPoint>> decode_tvs(std::span<const std::uint8_t> bytes) {
  if (bytes.size() % 12 != 0) return fail(ErrorKind::Protocol, "f32le-tvs/1: length is not a multiple of 12");
  std::vector<TvsPoint> out(bytes.size() / 12);
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = {get_le(&bytes[12 * i]), get_le(&bytes[12 * i + 4]), get_le(&bytes[12 * i + 8])};
  return out;
}

Sha256Digest blob_sha256(std::string_view codec, std::span<const std::uint8_t> bytes) {
  Sha256 h;
  h.update(codec);
  const std::uint8_t zero = 0;
  h.update(std::span(&zero, 1));
  h.update(bytes);
  return h.finish();
}

std::string encode_legacy_ff_base64(std::span<const TvPoint> points) {
  Bytes raw;
  raw.reserve(points.size() * 8);
  for (const auto& p : points) {
    put_be(raw, p.t);
    put_be(raw, p.v);
  }
  return base64_encode(raw);
}

Result<std::vector<TvPoint>> decode_legacy_ff_base64(std::string_view text) {
  auto raw = base64_decode(text);
  if (!raw) return fail(raw.error());
  if (raw->size() % 8 != 0) return fail(ErrorKind::Protocol, "legacy >ff: length is not a multiple of 8");
  std::vector<TvPoint> out(raw->size() / 8);
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = {get_be(&(*raw)[8 * i]), get_be(&(*raw)[8 * i + 4])};
  return out;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const std::uint32_t n = std::uint32_t(bytes[i]) << 16 | std::uint32_t(bytes[i + 1]) << 8 | bytes[i + 2];
    for (int s = 18; s >= 0; s -= 6) out.push_back(kB64[(n >> s) & 63]);
  }
  const std::size_t rem = bytes.size() - i;
  if (rem > 0) {
    std::uint32_t n = std::uint32_t(bytes[i]) << 16;
    if (rem == 2) n |= std::uint32_t(bytes[i + 1]) << 8;
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(rem == 2 ? kB64[(n >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

Result<Bytes> base64_decode(std::string_view text) {
  if (text.size() % 4 != 0) return fail(ErrorKind::Protocol, "base64: length is not a multiple of 4");
  Bytes out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    std::array<int, 4> v{};
    int pad = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      const char c = text[i + k];
      if (c == '=' && i + 4 == text.size() && k >= 2) {
        ++pad;
        v[k] = 0;
        continue;
      }
      // NOLINTNEXTLINE(bugprone-assignment-in-if-condition): decoded and tested in one step, on purpose
      if (pad > 0 || (v[k] = b64_value(c)) < 0) return fail(ErrorKind::Protocol, "base64: invalid character");
    }
    const std::uint32_t n = std::uint32_t(v[0]) << 18 | std::uint32_t(v[1]) << 12 | std::uint32_t(v[2]) << 6 |
                            std::uint32_t(v[3]);
    out.push_back(static_cast<std::uint8_t>(n >> 16));
    if (pad < 2) out.push_back(static_cast<std::uint8_t>(n >> 8));
    if (pad < 1) out.push_back(static_cast<std::uint8_t>(n));
  }
  return out;
}

}  // namespace pychron::persistence
