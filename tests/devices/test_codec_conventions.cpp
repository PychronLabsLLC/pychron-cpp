// The shared codec vocabulary (libs/codecs) lives with device_kit; vendor
// codecs get their own tests under tests/codecs.

#include "pychron/codecs/codec.hpp"

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::codec;

TEST(CodecConventions, CommandCarriesBytesAndReplyFraming) {
  Command c = Command::ascii("PR1\r\n", ReadSpec::until("\r\n"));
  EXPECT_EQ(c.tx, to_bytes("PR1\r\n"));
  ASSERT_TRUE(c.reply.has_value());
  EXPECT_EQ(*c.reply, ReadSpec::until("\r\n"));
  EXPECT_TRUE(c.expects_reply());

  Command w = Command::write_only(Bytes{0xFE, 0x01});
  EXPECT_FALSE(w.expects_reply());
  EXPECT_EQ(w.tx, (Bytes{0xFE, 0x01}));
}

TEST(CodecConventions, ProtocolErrorKindAndEscapedReply) {
  Error e = protocol_error("bad checksum", to_bytes("0,1\x07\r")).error();
  EXPECT_EQ(e.kind, ErrorKind::Protocol);
  EXPECT_EQ(e.what, "bad checksum: reply \"0,1\\x07\\r\"");
  EXPECT_TRUE(e.device.empty());  // drivers attribute, codecs do not know identity

  EXPECT_EQ(protocol_error("empty reply").error().what, "empty reply");
}

TEST(CodecConventions, ParseDecimal) {
  EXPECT_EQ(parse_decimal("1.23E-08"), 1.23e-8);
  EXPECT_EQ(parse_decimal("+7.6e+02"), 760.0);
  EXPECT_EQ(parse_decimal("-5"), -5.0);
  EXPECT_EQ(parse_decimal(".5"), 0.5);
  for (const char* bad : {"", ".", "E5", "1E", "1.0E-0x", "nan", "inf", "1,5", " 1", "1 ", "1e999"}) {
    EXPECT_EQ(parse_decimal(bad), std::nullopt) << bad;
  }
}

TEST(CodecConventions, StripTerminator) {
  auto ok = strip_terminator(to_bytes("1.0E-08\r\n"), "\r\n");
  ASSERT_TRUE(ok);
  EXPECT_EQ(*ok, "1.0E-08");

  auto missing = strip_terminator(to_bytes("1.0E-08"), "\r\n");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Protocol);

  auto only = strip_terminator(to_bytes("\r\n"), "\r\n");
  ASSERT_TRUE(only);
  EXPECT_EQ(*only, "");
}
