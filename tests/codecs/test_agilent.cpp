// codec::agilent: 34970A-family SCPI, as legacy pychron spoke it.
#include "pychron/codecs/agilent.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace ag = pychron::codec::agilent;

namespace {

template <class T>
void expect_protocol(const Result<T>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << to_string(r.error());
  EXPECT_TRUE(r.error().device.empty());
}

template <class T>
void expect_config(const Result<T>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config) << to_string(r.error());
}

}  // namespace

TEST(AgilentCodec, CommandsEndInLf) {
  EXPECT_EQ(ag::identify(), (codec::Command{to_bytes("*IDN?\n"), ag::reply_spec()}));
  EXPECT_EQ(ag::next_error(), (codec::Command{to_bytes("SYST:ERR?\n"), ag::reply_spec()}));
  EXPECT_EQ(ag::clear_status(), codec::Command::write_only(to_bytes("*CLS\n")));
  EXPECT_EQ(ag::reply_spec(), ReadSpec::until("\n"));
}

TEST(AgilentCodec, RouteCommandsAndQueries) {
  EXPECT_EQ(*ag::route_open("101"), codec::Command::write_only(to_bytes("ROUT:OPEN (@101)\n")));
  EXPECT_EQ(*ag::route_close(" 312 "), codec::Command::write_only(to_bytes("ROUT:CLOSE (@312)\n")));
  EXPECT_EQ(ag::query_open("101")->tx, to_bytes("ROUT:OPEN? (@101)\n"));
  EXPECT_EQ(ag::query_close("120")->tx, to_bytes("ROUT:CLOSE? (@120)\n"));
  EXPECT_EQ(ag::query_open("101")->reply, ag::reply_spec());
}

TEST(AgilentCodec, ChannelsAreSlotThenTwoDigits) {
  EXPECT_EQ(*ag::channel("101"), "101");
  EXPECT_EQ(*ag::channel("399"), "399");
  for (const char* bad : {"", "1", "10", "1010", "401", "001", "100", "1a1", "A", "101,102", "(@101)"}) {
    expect_config(ag::channel(bad));
    expect_config(ag::route_open(bad));
  }
}

TEST(AgilentCodec, Identity) {
  auto id = ag::decode_identity(to_bytes("Agilent Technologies,34970A,MY44012345,13-2-2\r\n"));
  ASSERT_TRUE(id) << id.error().what;
  EXPECT_EQ(*id, (ag::Identity{"Agilent Technologies", "34970A", "MY44012345", "13-2-2"}));
  EXPECT_TRUE(ag::is_switch_unit(*id));
  EXPECT_TRUE(ag::is_switch_unit({"Keysight Technologies", "DAQ970A", "x", "y"}));
  EXPECT_TRUE(ag::is_switch_unit({"HEWLETT-PACKARD", "34970A", "0", "1"}));
  EXPECT_FALSE(ag::is_switch_unit({"Agilent Technologies", "34401A", "x", "y"}));  // a DMM
  EXPECT_FALSE(ag::is_switch_unit({"LSCI", "MODEL335", "x", "y"}));
  expect_protocol(ag::decode_identity(to_bytes("Agilent,34970A\n")));
  expect_protocol(ag::decode_identity(to_bytes(",,,\n")));
}

TEST(AgilentCodec, RouteState) {
  EXPECT_EQ(*ag::decode_route_state(to_bytes("1\n")), true);
  EXPECT_EQ(*ag::decode_route_state(to_bytes("0\r\n")), false);
  // Legacy took any reply starting with '1' as yes; these are not answers.
  for (const char* bad : {"\n", "10\n", "1,0\n", "+1\n", "yes\n", "-113,\"Undefined header\"\n"}) {
    expect_protocol(ag::decode_route_state(to_bytes(bad)));
  }
}

TEST(AgilentCodec, ErrorQueue) {
  auto none = ag::decode_error(to_bytes("+0,\"No error\"\n"));
  ASSERT_TRUE(none);
  EXPECT_FALSE(none->has_value());
  auto e = ag::decode_error(to_bytes("-113,\"Undefined header\"\r\n"));
  ASSERT_TRUE(e);
  ASSERT_TRUE(e->has_value());
  EXPECT_EQ(**e, (ag::InstrumentError{-113, "Undefined header"}));
  auto plus = ag::decode_error(to_bytes("+221,\"Settings conflict\"\n"));
  ASSERT_TRUE(plus && plus->has_value());
  EXPECT_EQ((*plus)->code, 221);
  for (const char* bad : {"\n", "0\n", "x,\"y\"\n", "-113,Undefined header\n", "1234567,\"big\"\n"}) {
    expect_protocol(ag::decode_error(to_bytes(bad)));
  }
}

TEST(AgilentCodec, UnitSide) {
  auto r = ag::decode_request(to_bytes("ROUT:OPEN? (@101)\n"));
  ASSERT_TRUE(r);
  EXPECT_EQ(r->header, "ROUT:OPEN?");
  EXPECT_EQ(r->argument, "(@101)");
  EXPECT_EQ(ag::decode_request(to_bytes("*IDN?\n"))->argument, "");
  expect_protocol(ag::decode_request(to_bytes("\n")));
  EXPECT_EQ(ag::single_channel("(@101)"), "101");
  EXPECT_EQ(ag::single_channel("(@101,102)"), std::nullopt);
  EXPECT_EQ(ag::single_channel("(@101:105)"), std::nullopt);
  EXPECT_EQ(ag::single_channel("101"), std::nullopt);
  EXPECT_EQ(ag::encode_error(0, ""), to_bytes("+0,\"No error\"\n"));
  EXPECT_EQ(ag::encode_error(-113, "Undefined header"), to_bytes("-113,\"Undefined header\"\n"));
  EXPECT_EQ(ag::encode_line("1"), to_bytes("1\n"));
}
