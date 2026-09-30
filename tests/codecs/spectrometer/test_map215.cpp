#include "pychron/codecs/map215.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace map = pychron::codec::map215;

TEST(Map215Codec, SelectRangeIsWriteOnly) {
  auto c = map::select_range(3);
  ASSERT_TRUE(c);
  EXPECT_EQ(*c, codec::Command::write_only(to_bytes("B3.")));
  EXPECT_FALSE(c->expects_reply());
  EXPECT_EQ(map::select_range(0)->tx, to_bytes("B0."));
  EXPECT_EQ(map::select_range(9)->tx, to_bytes("B9."));
}

TEST(Map215Codec, WriteCodeIsWriteOnly) {
  EXPECT_EQ(*map::write_code(0), codec::Command::write_only(to_bytes("W0.")));
  EXPECT_EQ(*map::write_code(32768), codec::Command::write_only(to_bytes("W32768.")));
  EXPECT_EQ(*map::write_code(65535), codec::Command::write_only(to_bytes("W65535.")));
}

TEST(Map215Codec, OutOfRangeArgumentsAreConfigErrors) {
  for (int r : {-1, 10}) {
    auto c = map::select_range(r);
    ASSERT_FALSE(c) << r;
    EXPECT_EQ(c.error().kind, ErrorKind::Config);
  }
  for (std::int64_t code : {std::int64_t{-1}, std::int64_t{65536}}) {
    auto c = map::write_code(code);
    ASSERT_FALSE(c) << code;
    EXPECT_EQ(c.error().kind, ErrorKind::Config);
  }
}

TEST(Map215Codec, VoltsToCodeRoundsAndChecksBounds) {
  EXPECT_EQ(*map::to_code(0.0, 10.0), 0);
  EXPECT_EQ(*map::to_code(10.0, 10.0), 65535);
  EXPECT_EQ(*map::to_code(5.0, 10.0), 32768);  // 32767.5 rounds away from zero
  EXPECT_NEAR(map::to_volts(32768, 10.0), 5.0, 10.0 / 65535);
  EXPECT_DOUBLE_EQ(map::to_volts(65535, 10.0), 10.0);
  for (double v : {-0.001, 10.001}) {
    auto c = map::to_code(v, 10.0);
    ASSERT_FALSE(c) << v;
    EXPECT_EQ(c.error().kind, ErrorKind::Config);
  }
  EXPECT_FALSE(map::to_code(1.0, 0.0));
}

TEST(Map215Codec, DecodesRequests) {
  EXPECT_EQ(*map::decode_request(to_bytes("B2.")), (map::Request{map::Request::Kind::SelectRange, 2}));
  EXPECT_EQ(*map::decode_request(to_bytes("W1234.")), (map::Request{map::Request::Kind::Write, 1234}));
}

TEST(Map215Codec, RejectsGarbageRequests) {
  for (std::string_view tx : {"", "B.", "W12", "X1.", "W-1.", "W65536.", "B10.", "W1a."}) {
    auto r = map::decode_request(to_bytes(tx));
    ASSERT_FALSE(r) << tx;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << tx;
    EXPECT_TRUE(r.error().device.empty());
  }
}
