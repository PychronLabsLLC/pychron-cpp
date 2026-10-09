#include "pychron/persistence/blob.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

using namespace pychron::persistence;

namespace {
std::span<const std::uint8_t> as_bytes(std::string_view s) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the text's bytes
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}
}  // namespace

TEST(Blob, Base64Rfc4648Vectors) {
  const std::pair<std::string, std::string> vectors[] = {
      {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
      {"foobar", "Zm9vYmFy"}};
  for (const auto& [plain, encoded] : vectors) {
    EXPECT_EQ(base64_encode(as_bytes(plain)), encoded);
    const auto decoded = base64_decode(encoded);
    ASSERT_TRUE(decoded) << encoded;
    EXPECT_EQ(std::string(decoded->begin(), decoded->end()), plain);
  }
  EXPECT_FALSE(base64_decode("Zm9"));
  EXPECT_FALSE(base64_decode("Zm9*"));
  EXPECT_FALSE(base64_decode("Z=9v"));
}

TEST(Blob, TvLayoutIsLittleEndianInterleaved) {
  const TvPoint p[] = {{1.0f, -2.0f}};
  const Bytes b = encode_tv(p);
  const Bytes expected = {0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0xc0};
  EXPECT_EQ(b, expected);
}

TEST(Blob, TvAndTvsRoundTripExactly) {
  std::vector<TvPoint> tv;
  std::vector<TvsPoint> tvs;
  for (int i = 0; i < 100; ++i) {
    const float t = static_cast<float>(i) * 0.137f;
    tv.push_back({t, std::sin(t) * 1e-14f});
    tvs.push_back({t, std::cos(t), 1e-3f * static_cast<float>(i)});
  }
  tv.push_back({std::numeric_limits<float>::denorm_min(), -0.0f});
  EXPECT_EQ(*decode_tv(encode_tv(tv)), tv);
  EXPECT_EQ(*decode_tvs(encode_tvs(tvs)), tvs);
  const Bytes odd(7, 0);
  EXPECT_FALSE(decode_tv(odd));
  EXPECT_FALSE(decode_tvs(Bytes(8, 0)));
}

TEST(Blob, LegacyFfBase64RoundTripsByteIdentically) {
  // Legacy `.data.json` series -> f32le-tv/1 -> legacy is byte-identical (7.2).
  std::vector<TvPoint> points;
  for (int i = 0; i < 37; ++i) points.push_back({static_cast<float>(i) * 0.25f, 1.0f / static_cast<float>(i + 3)});
  const std::string legacy = encode_legacy_ff_base64(points);
  const auto imported = decode_legacy_ff_base64(legacy);
  ASSERT_TRUE(imported);
  const Bytes stored = encode_tv(*imported);
  const auto published = decode_tv(stored);
  ASSERT_TRUE(published);
  EXPECT_EQ(encode_legacy_ff_base64(*published), legacy);
}

TEST(Blob, LegacyIsBigEndian) {
  const TvPoint p[] = {{1.0f, -2.0f}};
  // 3f800000 c0000000
  EXPECT_EQ(encode_legacy_ff_base64(p), base64_encode(Bytes{0x3f, 0x80, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00}));
}

TEST(Blob, HashIsCodecNulBytes) {
  const Bytes bytes = {1, 2, 3};
  std::string joined = std::string(kCodecTv) + '\0' + "\x01\x02\x03";
  EXPECT_EQ(blob_sha256(kCodecTv, bytes), pychron::sha256(std::string_view{joined}));
  EXPECT_NE(blob_sha256(kCodecTv, bytes), blob_sha256(kCodecTvs, bytes));
}
