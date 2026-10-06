// codec::modbus. Frames are the Modbus Application Protocol spec's own
// examples (V1.1b3, section 6) in an MBAP header.
#include "pychron/codecs/modbus.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace pychron;
namespace mb = pychron::codec::modbus;
using mb::Function;
using mb::WordOrder;

namespace {

// Spaces only separate fields for the reader.
Bytes hex(std::string_view h) {
  std::string digits;
  for (char c : h)
    if (c != ' ') digits += c;
  auto b = from_hex(digits);
  EXPECT_TRUE(b) << h;
  return b ? *b : Bytes{};
}

template <class T>
void expect_protocol(const Result<T>& r, std::string_view needle = {}) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << to_string(r.error());
  EXPECT_TRUE(r.error().device.empty());
  if (!needle.empty()) {
    EXPECT_NE(r.error().what.find(needle), std::string::npos) << r.error().what;
  }
}

template <class T>
void expect_config(const Result<T>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config) << to_string(r.error());
}

}  // namespace

// --- requests ------------------------------------------------------------------

TEST(ModbusCodec, ReadCoilsFrame) {
  // Spec 6.1: coils 20..38 (start 0x13, count 0x13).
  auto c = mb::read_coils(0x0001, 0x01, 0x0013, 0x0013);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->tx, hex("000100000006010100130013"));
  EXPECT_EQ(c->reply, ReadSpec::modbus_tcp());
}

TEST(ModbusCodec, ReadRegistersFrames) {
  // Spec 6.3: holding registers 108..110 (start 0x6B, count 3).
  EXPECT_EQ(mb::read_holding_registers(0x0A0B, 0x11, 0x006B, 3)->tx, hex("0A0B000000061103006B0003"));
  // Spec 6.4: input register 9 (start 0x08, count 1).
  EXPECT_EQ(mb::read_input_registers(0x0002, 0x11, 0x0008, 1)->tx, hex("000200000006110400080001"));
}

TEST(ModbusCodec, WriteSingleCoilFrame) {
  // Spec 6.5: coil 173 on.
  auto on = mb::write_single_coil(0x0003, 0x01, 0x00AC, true);
  EXPECT_EQ(on.tx, hex("00030000000601 05 00AC FF00"));
  EXPECT_EQ(mb::write_single_coil(0x0003, 0x01, 0x00AC, false).tx, hex("000300000006010500AC0000"));
}

TEST(ModbusCodec, WriteMultipleRegistersFrame) {
  // Spec 6.12: registers 2..3 = 0x000A, 0x0102.
  auto c = mb::write_multiple_registers(0x0004, 0x01, 0x0001, {0x000A, 0x0102});
  ASSERT_TRUE(c);
  EXPECT_EQ(c->tx, hex("00040000000B01 10 0001 0002 04 000A 0102"));
}

TEST(ModbusCodec, CountsAndRangesOutsideTheLimitsAreConfigErrors) {
  expect_config(mb::read_coils(1, 1, 0, 0));
  expect_config(mb::read_coils(1, 1, 0, 2001));
  EXPECT_TRUE(mb::read_coils(1, 1, 0, 2000));
  expect_config(mb::read_holding_registers(1, 1, 0, 126));
  expect_config(mb::read_input_registers(1, 1, 0xFFFF, 2));
  EXPECT_TRUE(mb::read_input_registers(1, 1, 0xFFFF, 1));
  expect_config(mb::write_multiple_registers(1, 1, 0, {}));
  expect_config(mb::write_multiple_registers(1, 1, 0, std::vector<std::uint16_t>(124, 0)));
  expect_config(mb::write_multiple_registers(1, 1, 0xFFFF, {1, 2}));
}

// --- replies -------------------------------------------------------------------

TEST(ModbusCodec, DecodesCoils) {
  // Spec 6.1: CD 6B 05 for coils 20..38.
  auto coils = mb::decode_coils(0x0001, 0x01, 19, hex("000100000006010103CD6B05"));
  ASSERT_TRUE(coils) << coils.error().what;
  ASSERT_EQ(coils->size(), 19u);
  // 0xCD = 1100 1101: coil 20 is the LSB.
  const std::vector<bool> first8{true, false, true, true, false, false, true, true};
  EXPECT_EQ(std::vector<bool>(coils->begin(), coils->begin() + 8), first8);
  // 0x05: coils 36..38 are 1, 0, 1.
  EXPECT_TRUE((*coils)[16]);
  EXPECT_FALSE((*coils)[17]);
  EXPECT_TRUE((*coils)[18]);
}

TEST(ModbusCodec, DecodesRegisters) {
  // Spec 6.3: 0x022B, 0x0000, 0x0064.
  auto regs = mb::decode_registers(0x0A0B, 0x11, Function::ReadHoldingRegisters, 3,
                                   hex("0A0B00000009110306022B00000064"));
  ASSERT_TRUE(regs) << regs.error().what;
  EXPECT_EQ(*regs, (std::vector<std::uint16_t>{0x022B, 0x0000, 0x0064}));
  auto input = mb::decode_registers(2, 0x11, Function::ReadInputRegisters, 1, hex("000200000005110402000A"));
  ASSERT_TRUE(input);
  EXPECT_EQ(*input, (std::vector<std::uint16_t>{0x000A}));
}

TEST(ModbusCodec, AReplyToAnotherFunctionIsRejected) {
  // An input-register reply where a holding-register read was asked.
  expect_protocol(mb::decode_registers(2, 0x11, Function::ReadHoldingRegisters, 1, hex("000200000005110402000A")),
                  "unexpected function");
  EXPECT_EQ(mb::decode_registers(2, 0x11, Function::ReadCoils, 1, hex("000200000005110402000A")).error().kind,
            ErrorKind::Config);
}

TEST(ModbusCodec, AReplyMeantForAnotherRequestIsRejected) {
  const Bytes reply = hex("000700000005110402000A");
  expect_protocol(mb::decode_registers(8, 0x11, Function::ReadInputRegisters, 1, reply), "transaction id");
  expect_protocol(mb::decode_registers(7, 0x12, Function::ReadInputRegisters, 1, reply), "unit id");
  expect_protocol(mb::decode_registers(7, 0x11, Function::ReadInputRegisters, 2, reply), "byte count");
}

TEST(ModbusCodec, MalformedFramesAreProtocolErrors) {
  expect_protocol(mb::decode_coils(1, 1, 8, hex("0001000000")), "short");
  expect_protocol(mb::decode_coils(1, 1, 8, hex("000100010004010101FF")), "not Modbus");
  expect_protocol(mb::decode_coils(1, 1, 8, hex("000100000009010101FF")), "length");
  expect_protocol(mb::decode_coils(1, 1, 8, hex("000100000004010102FF")), "byte count");
}

TEST(ModbusCodec, ExceptionsSayTheirCodeAndName) {
  auto r = mb::decode_coils(1, 1, 8, hex("0001000000030181 02"));
  expect_protocol(r, "exception 2 (illegal data address)");
  expect_protocol(mb::decode_write_single_coil(1, 1, 5, true, hex("00010000000301 85 04")),
                  "exception 4 (device failure)");
  expect_protocol(mb::decode_registers(1, 1, Function::ReadInputRegisters, 1, hex("00010000000301 84 63")),
                  "exception 99 (unlisted exception)");
  EXPECT_EQ(mb::exception_name(1), "illegal function");
}

TEST(ModbusCodec, WriteEchoesMustMatch) {
  EXPECT_TRUE(mb::decode_write_single_coil(3, 1, 0xAC, true, hex("000300000006010500ACFF00")));
  expect_protocol(mb::decode_write_single_coil(3, 1, 0xAC, false, hex("000300000006010500ACFF00")), "echo");
  expect_protocol(mb::decode_write_single_coil(3, 1, 0xAD, true, hex("000300000006010500ACFF00")), "echo");
  EXPECT_TRUE(mb::decode_write_multiple_registers(4, 1, 1, 2, hex("000400000006011000010002")));
  expect_protocol(mb::decode_write_multiple_registers(4, 1, 1, 3, hex("000400000006011000010002")), "echo");
}

// --- 32-bit values ---------------------------------------------------------------

TEST(ModbusCodec, FloatInEveryWordOrder) {
  // 123.456f = 0x42F6E979: A=42 B=F6 C=E9 D=79.
  const float v = 123.456F;
  EXPECT_EQ(mb::encode_float(v, WordOrder::ABCD), (std::array<std::uint16_t, 2>{0x42F6, 0xE979}));
  EXPECT_EQ(mb::encode_float(v, WordOrder::CDAB), (std::array<std::uint16_t, 2>{0xE979, 0x42F6}));
  EXPECT_EQ(mb::encode_float(v, WordOrder::BADC), (std::array<std::uint16_t, 2>{0xF642, 0x79E9}));
  EXPECT_EQ(mb::encode_float(v, WordOrder::DCBA), (std::array<std::uint16_t, 2>{0x79E9, 0xF642}));
  for (auto order : {WordOrder::ABCD, WordOrder::CDAB, WordOrder::BADC, WordOrder::DCBA}) {
    const auto w = mb::encode_float(v, order);
    EXPECT_EQ(mb::decode_float(w[0], w[1], order), v) << mb::to_string(order);
  }
  // Read in the wrong order, a plausible number comes out: the order is config, never guessed.
  EXPECT_NE(mb::decode_float(0xE979, 0x42F6, WordOrder::ABCD), v);
}

TEST(ModbusCodec, Int32InEveryWordOrder) {
  // -2 = 0xFFFFFFFE; 0x01020304 shows every byte.
  EXPECT_EQ(mb::encode_int32(-2, WordOrder::CDAB), (std::array<std::uint16_t, 2>{0xFFFE, 0xFFFF}));
  EXPECT_EQ(mb::encode_int32(0x01020304, WordOrder::ABCD), (std::array<std::uint16_t, 2>{0x0102, 0x0304}));
  EXPECT_EQ(mb::encode_int32(0x01020304, WordOrder::CDAB), (std::array<std::uint16_t, 2>{0x0304, 0x0102}));
  EXPECT_EQ(mb::encode_int32(0x01020304, WordOrder::BADC), (std::array<std::uint16_t, 2>{0x0201, 0x0403}));
  EXPECT_EQ(mb::encode_int32(0x01020304, WordOrder::DCBA), (std::array<std::uint16_t, 2>{0x0403, 0x0201}));
  for (auto order : {WordOrder::ABCD, WordOrder::CDAB, WordOrder::BADC, WordOrder::DCBA}) {
    for (std::int32_t v : {0, 1, -1, -2, 300, 2147483647, -2147483647 - 1}) {
      const auto w = mb::encode_int32(v, order);
      EXPECT_EQ(mb::decode_int32(w[0], w[1], order), v);
    }
  }
}

TEST(ModbusCodec, WordOrderNames) {
  EXPECT_EQ(mb::word_order_from_string("CDAB"), WordOrder::CDAB);
  EXPECT_EQ(mb::word_order_from_string("dcba"), WordOrder::DCBA);
  EXPECT_EQ(mb::word_order_from_string("little"), std::nullopt);
  EXPECT_EQ(mb::word_order_from_string(""), std::nullopt);
  EXPECT_EQ(mb::to_string(WordOrder::BADC), "badc");
}

// --- device side ------------------------------------------------------------------

TEST(ModbusCodec, DecodesEveryRequestItBuilds) {
  auto coils = mb::decode_request(mb::read_coils(9, 2, 0x13, 0x13)->tx);
  ASSERT_TRUE(coils);
  EXPECT_EQ(*coils, (mb::Request{9, 2, 0x01, 0x13, 0x13, false, {}}));
  auto holding = mb::decode_request(mb::read_holding_registers(9, 2, 7, 4)->tx);
  ASSERT_TRUE(holding);
  EXPECT_EQ(holding->function, 0x03);
  EXPECT_EQ(holding->count, 4);
  auto coil = mb::decode_request(mb::write_single_coil(9, 2, 0xAC, true).tx);
  ASSERT_TRUE(coil);
  EXPECT_EQ(*coil, (mb::Request{9, 2, 0x05, 0xAC, 1, true, {}}));
  auto regs = mb::decode_request(mb::write_multiple_registers(9, 2, 1, {0x000A, 0x0102})->tx);
  ASSERT_TRUE(regs);
  EXPECT_EQ(*regs, (mb::Request{9, 2, 0x10, 1, 2, false, {0x000A, 0x0102}}));
}

TEST(ModbusCodec, RefusesRequestsItCannotRead) {
  expect_protocol(mb::decode_request(hex("0001000000060102001300")), "short");  // truncated
  expect_protocol(mb::decode_request(hex("000100000006010600010003")), "unsupported");  // write single register
  EXPECT_EQ(mb::request_function(hex("000100000006010600010003")), 0x06);
  expect_protocol(mb::decode_request(hex("000100000006010500AC1234")), "FF00");
  expect_protocol(mb::decode_request(hex("000100000009011000010002040001")), "length");
  EXPECT_EQ(mb::request_function(hex("0001")), std::nullopt);
}

TEST(ModbusCodec, DeviceRepliesRoundTrip) {
  const std::vector<bool> coils{true, false, true, true, false, false, true, true, false, true};
  EXPECT_EQ(*mb::decode_coils(5, 3, 10, mb::encode_coils(5, 3, coils)), coils);
  EXPECT_EQ(mb::encode_coils(1, 1, {true, false, true, true, false, false, true, true}), hex("000100000004010101CD"));
  const std::vector<std::uint16_t> regs{0x022B, 0x0000, 0x0064};
  EXPECT_EQ(mb::encode_registers(0x0A0B, 0x11, Function::ReadHoldingRegisters, regs),
            hex("0A0B00000009110306022B00000064"));
  EXPECT_TRUE(mb::decode_write_single_coil(3, 1, 0xAC, true, mb::encode_write_single_coil(3, 1, 0xAC, true)));
  EXPECT_TRUE(mb::decode_write_multiple_registers(4, 1, 1, 2, mb::encode_write_multiple_registers(4, 1, 1, 2)));
  EXPECT_EQ(mb::encode_exception(1, 1, 0x01, 2), hex("000100000003018102"));
}
