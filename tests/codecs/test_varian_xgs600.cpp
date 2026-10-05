#include "pychron/codecs/varian_xgs600.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace xgs = pychron::codec::varian_xgs600;

TEST(Xgs600Codec, ReadPressureByLabel) {
  EXPECT_EQ(*xgs::read_pressure("00", "IG1"), (codec::Command{to_bytes("#0002UIG1\r"), ReadSpec::until("\r")}));
  EXPECT_EQ(xgs::read_pressure("0A", "CNV1")->tx, to_bytes("#0A02UCNV1\r"));
  for (auto [a, l] : {std::pair{"0", "IG1"}, {"000", "IG1"}, {"ZZ", "IG1"}, {"00", ""}, {"00", "IG 1"},
                      {"00", "LONGLABEL9"}, {"00", "IG1\r"}}) {
    auto r = xgs::read_pressure(a, l);
    ASSERT_FALSE(r) << a << " " << l;
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
  }
}

TEST(Xgs600Codec, DecodesPressure) {
  EXPECT_DOUBLE_EQ(*xgs::decode_pressure(to_bytes(">1.234E-07\r")), 1.234e-7);
  EXPECT_DOUBLE_EQ(*xgs::decode_pressure(to_bytes(">7.60E+02\r")), 760.0);
}

TEST(Xgs600Codec, NonNumbersAreProtocolErrors) {
  // Never a number in place of one (legacy kept the last value instead).
  struct Case {
    const char* reply;
    const char* says;
  };
  for (auto c : {Case{">OFF\r", "gauge off"}, Case{"?FF\r", "rejected"}, Case{"1.0E-07\r", "expected '>'"},
                 Case{">\r", "not a pressure"}, Case{">-1.0E-07\r", "not a pressure"}, Case{">nan\r", "not a pressure"},
                 Case{"\r", "expected '>'"}}) {
    auto r = xgs::decode_pressure(to_bytes(c.reply));
    ASSERT_FALSE(r) << c.reply;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
    EXPECT_NE(r.error().what.find(c.says), std::string::npos) << r.error().what;
    EXPECT_TRUE(r.error().device.empty());
  }
}

TEST(Xgs600Codec, ControllerSide) {
  EXPECT_EQ(xgs::requested_label("00", to_bytes("#0002UIG1\r")), "IG1");
  EXPECT_EQ(xgs::requested_label("01", to_bytes("#0002UIG1\r")), std::nullopt);
  EXPECT_EQ(xgs::requested_label("00", to_bytes("#0002XIG1\r")), std::nullopt);
  EXPECT_EQ(xgs::encode_pressure(1.234e-7), to_bytes(">1.234E-07\r"));
  EXPECT_DOUBLE_EQ(*xgs::decode_pressure(xgs::encode_pressure(3.3e-9)), 3.3e-9);
  EXPECT_EQ(xgs::encode_off(), to_bytes(">OFF\r"));
  EXPECT_EQ(xgs::encode_rejected(), to_bytes("?FF\r"));
}
