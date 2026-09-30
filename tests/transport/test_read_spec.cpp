#include "pychron/transport/read_spec.hpp"

#include <gtest/gtest.h>

using namespace pychron;

TEST(ReadSpec, TerminatorFindsFirstOccurrence) {
  const auto spec = ReadSpec::until("\r\n");
  EXPECT_FALSE(frame_length(spec, to_bytes("PR1")).has_value());
  EXPECT_FALSE(frame_length(spec, to_bytes("PR1\r")).has_value());
  EXPECT_EQ(frame_length(spec, to_bytes("PR1\r\nextra\r\n")), 5u);
}

TEST(ReadSpec, EmptyTerminatorIsImmediatelyComplete) {
  EXPECT_EQ(frame_length(ReadSpec::until(Bytes{}), Bytes{}), 0u);
}

TEST(ReadSpec, FixedLength) {
  const auto spec = ReadSpec::fixed(3);
  EXPECT_FALSE(frame_length(spec, Bytes{1, 2}).has_value());
  EXPECT_EQ(frame_length(spec, Bytes{1, 2, 3, 4}), 3u);
}

TEST(ReadSpec, ModbusRtuReadRegistersUsesByteCount) {
  const auto spec = ReadSpec::modbus_rtu();
  // addr, fn 0x03, byte count 4, 4 data bytes, 2 crc = 9
  Bytes frame{0x01, 0x03, 0x04, 0, 1, 0, 2, 0xAA, 0xBB};
  EXPECT_EQ(frame_length(spec, frame), 9u);
  frame.pop_back();
  EXPECT_FALSE(frame_length(spec, frame).has_value());
  EXPECT_FALSE(frame_length(spec, Bytes{0x01, 0x03}).has_value());
}

TEST(ReadSpec, ModbusRtuWriteEchoIsEightBytes) {
  const Bytes frame{0x01, 0x06, 0x00, 0x10, 0x00, 0x01, 0xCC, 0xDD};
  EXPECT_EQ(frame_length(ReadSpec::modbus_rtu(), frame), 8u);
  const Bytes multi{0x01, 0x10, 0x00, 0x10, 0x00, 0x02, 0xCC, 0xDD};
  EXPECT_EQ(frame_length(ReadSpec::modbus_rtu(), multi), 8u);
}

TEST(ReadSpec, ModbusRtuExceptionIsFiveBytes) {
  const Bytes frame{0x01, 0x83, 0x02, 0xC0, 0xF1};
  EXPECT_EQ(frame_length(ReadSpec::modbus_rtu(), frame), 5u);
}

TEST(ReadSpec, ModbusTcpUsesMbapLength) {
  // txn 0x0001, proto 0, length 5 (unit + fn + 3 bytes), unit 1
  const Bytes frame{0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x01, 0x03, 0x02, 0x00, 0x07};
  EXPECT_EQ(frame_length(ReadSpec::modbus_tcp(), frame), 11u);
  EXPECT_FALSE(frame_length(ReadSpec::modbus_tcp(), Bytes(frame.begin(), frame.begin() + 10)).has_value());
  EXPECT_FALSE(frame_length(ReadSpec::modbus_tcp(), Bytes{0x00, 0x01}).has_value());
}
