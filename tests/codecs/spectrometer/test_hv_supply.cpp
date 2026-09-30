#include "pychron/codecs/hv_supply.hpp"

#include <gtest/gtest.h>

#include <limits>

using namespace pychron;
namespace hv = pychron::codec::hv_supply;

namespace {
const ReadSpec kLine = ReadSpec::until("\r");
}

TEST(HvSupplyCodec, Commands) {
  EXPECT_EQ(*hv::set_voltage(4500.0), codec::Command::ascii("VSET 4500.0\r", kLine));
  EXPECT_EQ(hv::set_voltage(4499.96)->tx, to_bytes("VSET 4500.0\r"));
  EXPECT_EQ(hv::set_voltage(0.0)->tx, to_bytes("VSET 0.0\r"));
  EXPECT_EQ(hv::read_setpoint(), codec::Command::ascii("VSET?\r", kLine));
  EXPECT_EQ(hv::read_output(), codec::Command::ascii("VOUT?\r", kLine));
}

TEST(HvSupplyCodec, InvalidSetpointsAreConfigErrors) {
  for (double v : {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    auto c = hv::set_voltage(v);
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().kind, ErrorKind::Config);
  }
}

TEST(HvSupplyCodec, DecodesReplies) {
  EXPECT_TRUE(hv::decode_ok(to_bytes("OK\r")));
  EXPECT_DOUBLE_EQ(*hv::decode_voltage(to_bytes("4499.8\r")), 4499.8);
  EXPECT_DOUBLE_EQ(*hv::decode_voltage(hv::encode_voltage(1234.56)), 1234.6);
}

TEST(HvSupplyCodec, RejectsBadReplies) {
  for (std::string_view reply : {"OK", "ERR RANGE\r", "NO\r"}) {
    auto r = hv::decode_ok(to_bytes(reply));
    ASSERT_FALSE(r) << reply;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
    EXPECT_TRUE(r.error().device.empty());
  }
  for (std::string_view reply : {"4500", "ERR RANGE\r", "abc\r", "\r"}) {
    auto r = hv::decode_voltage(to_bytes(reply));
    ASSERT_FALSE(r) << reply;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  }
}

TEST(HvSupplyCodec, DecodesRequests) {
  using K = hv::Request::Kind;
  EXPECT_EQ(*hv::decode_request(to_bytes("VSET 4500.0\r")), (hv::Request{K::Set, 4500.0}));
  EXPECT_EQ(hv::decode_request(to_bytes("VSET?\r"))->kind, K::ReadSetpoint);
  EXPECT_EQ(hv::decode_request(to_bytes("VOUT?\r"))->kind, K::ReadOutput);
  for (std::string_view tx : {"VSET -1\r", "VSET x\r", "HV?\r", "VOUT?"}) {
    auto r = hv::decode_request(to_bytes(tx));
    ASSERT_FALSE(r) << tx;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  }
  EXPECT_EQ(hv::encode_error("RANGE"), to_bytes("ERR RANGE\r"));
}
