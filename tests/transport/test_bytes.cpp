#include "pychron/transport/bytes.hpp"

#include <gtest/gtest.h>

using namespace pychron;

TEST(Bytes, RoundTripsString) {
  const Bytes b = to_bytes(std::string_view("PR1\r\n", 5));
  ASSERT_EQ(b.size(), 5u);
  EXPECT_EQ(b[3], '\r');
  EXPECT_EQ(to_string(b), "PR1\r\n");
}

TEST(Bytes, HexRoundTrip) {
  const Bytes b{0x00, 0x01, 0xab, 0xff};
  EXPECT_EQ(to_hex(b), "0001abff");
  EXPECT_EQ(from_hex("0001ABff"), b);
  EXPECT_EQ(from_hex(""), Bytes{});
}

TEST(Bytes, FromHexRejectsMalformed) {
  EXPECT_FALSE(from_hex("abc").has_value());
  EXPECT_FALSE(from_hex("zz").has_value());
}

TEST(Bytes, EscapeRendersControlBytes) {
  EXPECT_EQ(escape(to_bytes("OK\r\n")), "OK\\r\\n");
  EXPECT_EQ(escape(Bytes{0x06, 'A'}), "\\x06A");
}
