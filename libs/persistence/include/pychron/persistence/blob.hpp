#pragma once

// Raw signal codecs and content addressing (DVC schema spec, section 7.2).
//
//   f32le-tv/1    little-endian float32, interleaved (t, v)
//   f32le-tvs/1   little-endian float32, interleaved (t, v, sigma)
//   sha256        SHA-256(codec || 0x00 || bytes)
//
// The legacy `.data.json` form is base64 of big-endian float32 (t, v) pairs
// (`struct.pack('>ff', t, v)` per point). float32 converts exactly in both
// directions, so legacy -> f32le-tv/1 -> legacy is byte-identical.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/sha256.hpp"

namespace pychron::persistence {

inline constexpr std::string_view kCodecTv = "f32le-tv/1";
inline constexpr std::string_view kCodecTvs = "f32le-tvs/1";

struct TvPoint {
  float t = 0;
  float v = 0;
  friend bool operator==(const TvPoint&, const TvPoint&) = default;
};

struct TvsPoint {
  float t = 0;
  float v = 0;
  float s = 0;
  friend bool operator==(const TvsPoint&, const TvsPoint&) = default;
};

using Bytes = std::vector<std::uint8_t>;

Bytes encode_tv(std::span<const TvPoint> points);
Result<std::vector<TvPoint>> decode_tv(std::span<const std::uint8_t> bytes);
Bytes encode_tvs(std::span<const TvsPoint> points);
Result<std::vector<TvsPoint>> decode_tvs(std::span<const std::uint8_t> bytes);

// The content address of a blob; the server recomputes and checks it (I8).
Sha256Digest blob_sha256(std::string_view codec, std::span<const std::uint8_t> bytes);

// Legacy `.data.json` series: base64 of `>ff` pairs.
std::string encode_legacy_ff_base64(std::span<const TvPoint> points);
Result<std::vector<TvPoint>> decode_legacy_ff_base64(std::string_view text);

// RFC 4648 base64 with padding.
std::string base64_encode(std::span<const std::uint8_t> bytes);
Result<Bytes> base64_decode(std::string_view text);

}  // namespace pychron::persistence
