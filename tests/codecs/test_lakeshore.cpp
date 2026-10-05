#include "pychron/codecs/lakeshore.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <locale>

using namespace pychron;
namespace ls = pychron::codec::lakeshore;

namespace {

template <class T>
void expect_config(const Result<T>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config) << to_string(r.error());
}

}  // namespace

TEST(LakeshoreCodec, Commands) {
  EXPECT_EQ(ls::identify(), (codec::Command{to_bytes("*IDN?\n"), ReadSpec::until("\n")}));
  EXPECT_EQ(ls::clear_status(), codec::Command::write_only(to_bytes("*CLS\n")));
  EXPECT_EQ(ls::read_input('a')->tx, to_bytes("KRDG? A\n"));  // legacy sent lower case
  EXPECT_EQ(ls::read_input('C', ls::Units::Celsius)->tx, to_bytes("CRDG? C\n"));
  EXPECT_EQ(*ls::set_setpoint(1, 77), codec::Command::write_only(to_bytes("SETP 1,77.000\n")));
  EXPECT_EQ(ls::set_setpoint(2, 14.25)->tx, to_bytes("SETP 2,14.250\n"));
  EXPECT_EQ(ls::query_setpoint(1)->tx, to_bytes("SETP? 1\n"));
  EXPECT_EQ(*ls::set_range(1, 0), codec::Command::write_only(to_bytes("RANGE 1,0\n")));
  EXPECT_EQ(ls::query_range(2)->tx, to_bytes("RANGE? 2\n"));
}

TEST(LakeshoreCodec, ArgumentsOutOfRangeAreConfig) {
  expect_config(ls::read_input('E'));
  expect_config(ls::read_input('1'));
  expect_config(ls::set_setpoint(0, 77));
  expect_config(ls::set_setpoint(5, 77));
  expect_config(ls::set_setpoint(1, -1));
  expect_config(ls::set_setpoint(1, NAN));
  expect_config(ls::set_setpoint(1, 2000.5));
  expect_config(ls::set_range(1, 6));
  expect_config(ls::set_range(1, -1));
  expect_config(ls::query_setpoint(9));
}

TEST(LakeshoreCodec, SetpointsIgnoreTheLocale) {
  // A comma-decimal locale must not turn 77.5 into "77,500".
  try {
    std::locale::global(std::locale("de_DE.UTF-8"));
  } catch (...) {
  }
  EXPECT_EQ(ls::set_setpoint(1, 77.5)->tx, to_bytes("SETP 1,77.500\n"));
  std::locale::global(std::locale::classic());
}

TEST(LakeshoreCodec, Replies) {
  EXPECT_DOUBLE_EQ(*ls::decode_number(to_bytes("+077.123\r\n")), 77.123);
  EXPECT_DOUBLE_EQ(*ls::decode_number(to_bytes("-012.500\r\n")), -12.5);
  EXPECT_EQ(*ls::decode_range(to_bytes("2\r\n")), 2);
  for (const char* bad : {"T.OVER\r\n", "\r\n", "+--\r\n"}) {
    auto r = ls::decode_number(to_bytes(bad));
    ASSERT_FALSE(r) << bad;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  }
  EXPECT_FALSE(ls::decode_range(to_bytes("7\r\n")));
  EXPECT_FALSE(ls::decode_range(to_bytes("12\r\n")));
  auto id = ls::decode_identity(to_bytes("LSCI,MODEL335,1234567/1234567,1.0\r\n"));
  ASSERT_TRUE(id);
  EXPECT_EQ(*id, (ls::Identity{"LSCI", "MODEL335", "1234567/1234567", "1.0"}));
  EXPECT_FALSE(ls::decode_identity(to_bytes("LSCI,MODEL335\r\n")));
}

TEST(LakeshoreCodec, UnitSide) {
  auto r = ls::decode_request(to_bytes("setp 1,77.000\n"));
  ASSERT_TRUE(r);
  EXPECT_EQ(r->header, "SETP");
  EXPECT_EQ(r->argument, "1,77.000");
  EXPECT_EQ(ls::encode_number(77.123), to_bytes("+077.123\r\n"));
  EXPECT_EQ(ls::encode_number(4.2), to_bytes("+004.200\r\n"));
  EXPECT_EQ(ls::encode_number(-12.5), to_bytes("-012.500\r\n"));
  EXPECT_DOUBLE_EQ(*ls::decode_number(ls::encode_number(293.15)), 293.15);
}
