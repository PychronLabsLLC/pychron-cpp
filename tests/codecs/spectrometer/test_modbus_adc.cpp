#include "pychron/codecs/modbus_adc.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace adc = pychron::codec::modbus_adc;

namespace {

Bytes hex(std::string_view h) { return *from_hex(h); }

void expect_protocol(const Error& e) {
  EXPECT_EQ(e.kind, ErrorKind::Protocol) << to_string(e);
  EXPECT_TRUE(e.device.empty());
}

}  // namespace

TEST(ModbusAdcCodec, ReadInputRegistersFrame) {
  auto c = adc::read_input_registers(0x0102, 0x11, 0x0010, 6);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->tx, hex("010200000006110400100006"));
  EXPECT_EQ(c->reply, ReadSpec::modbus_tcp());
}

TEST(ModbusAdcCodec, ReadChannelsUsesTwoRegistersEach) {
  EXPECT_EQ(adc::read_channels(1, 1, 0, 3)->tx, hex("000100000006010400000006"));
}

TEST(ModbusAdcCodec, BadCountsAreConfigErrors) {
  EXPECT_EQ(adc::read_input_registers(1, 1, 0, 0).error().kind, ErrorKind::Config);
  EXPECT_EQ(adc::read_input_registers(1, 1, 0, 126).error().kind, ErrorKind::Config);
  EXPECT_EQ(adc::read_input_registers(1, 1, 0xFFFF, 2).error().kind, ErrorKind::Config);
  EXPECT_EQ(adc::read_channels(1, 1, 0, 0).error().kind, ErrorKind::Config);
  EXPECT_EQ(adc::read_channels(1, 1, 0, 63).error().kind, ErrorKind::Config);
}

TEST(ModbusAdcCodec, FloatRegistersAreHighWordFirst) {
  // 1.5f = 0x3FC00000
  EXPECT_EQ(adc::to_registers(1.5F), (std::vector<std::uint16_t>{0x3FC0, 0x0000}));
  EXPECT_FLOAT_EQ(adc::to_float(0x3FC0, 0x0000), 1.5F);
  EXPECT_FLOAT_EQ(adc::to_float(0xC120, 0x0000), -10.0F);
}

TEST(ModbusAdcCodec, DecodesChannelValues) {
  // tid 7, unit 1, 2 channels: 1.5, -10.0
  auto reply = hex("00070000000b0104083fc00000c1200000");
  auto v = adc::decode_channels(7, 1, 2, reply);
  ASSERT_TRUE(v) << to_string(v.error());
  EXPECT_EQ(*v, (std::vector<double>{1.5, -10.0}));
  EXPECT_EQ(adc::encode_registers(7, 1, {0x3FC0, 0, 0xC120, 0}), reply);
}

TEST(ModbusAdcCodec, RejectsMismatchedOrMalformedReplies) {
  auto good = hex("00070000000b0104083fc00000c1200000");
  expect_protocol(adc::decode_channels(8, 1, 2, good).error());                          // tid
  expect_protocol(adc::decode_channels(7, 2, 2, good).error());                          // unit
  expect_protocol(adc::decode_channels(7, 1, 3, good).error());                          // count
  expect_protocol(adc::decode_channels(7, 1, 2, hex("000700000007")).error());           // short
  expect_protocol(adc::decode_channels(7, 1, 2, hex("0007000000080104083fc00000c1200000")).error());  // length
  expect_protocol(adc::decode_channels(7, 1, 2, hex("00070000000b0103083fc00000c1200000")).error());  // fn
  // NaN value
  expect_protocol(adc::decode_channels(7, 1, 1, hex("000700000007010404" "7fc00000")).error());
}

TEST(ModbusAdcCodec, ExceptionReplyIsProtocolError) {
  auto reply = adc::encode_exception(3, 1, adc::kReadInputRegisters, adc::kIllegalAddress);
  EXPECT_EQ(reply, hex("000300000003018402"));
  auto r = adc::decode_registers(3, 1, 2, reply);
  ASSERT_FALSE(r);
  expect_protocol(r.error());
  EXPECT_NE(r.error().what.find("exception 2"), std::string::npos) << r.error().what;
}

TEST(ModbusAdcCodec, DecodesRequests) {
  auto tx = adc::read_channels(9, 4, 0x20, 3)->tx;
  auto r = adc::decode_request(tx);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, (adc::Request{9, 4, adc::kReadInputRegisters, 0x20, 6}));
  expect_protocol(adc::decode_request(hex("0009")).error());
}
