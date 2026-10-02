#include "pychron/codecs/microion.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace mi = pychron::codec::microion;

namespace {

const ReadSpec kLine = ReadSpec::until("\r");

void expect_protocol(const Error& e, std::string_view fragment = {}) {
  EXPECT_EQ(e.kind, ErrorKind::Protocol) << to_string(e);
  EXPECT_TRUE(e.device.empty());
  if (!fragment.empty()) {
    EXPECT_NE(e.what.find(fragment), std::string::npos) << e.what;
  }
}

}  // namespace

// --- encode -------------------------------------------------------------------

TEST(MicroIonCodec, ReadPressureEncodesEachSensor) {
  EXPECT_EQ(*mi::read_pressure(1, 1), codec::Command::ascii("#01DS IG\r", kLine));
  EXPECT_EQ(*mi::read_pressure(1, 2), codec::Command::ascii("#01DS CG1\r", kLine));
  EXPECT_EQ(*mi::read_pressure(1, 3), codec::Command::ascii("#01DS CG2\r", kLine));
}

TEST(MicroIonCodec, AddressIsTwoUppercaseHexDigits) {
  EXPECT_EQ(mi::read_pressure(0x00, 1)->tx, to_bytes("#00DS IG\r"));
  EXPECT_EQ(mi::read_pressure(0x2A, 1)->tx, to_bytes("#2ADS IG\r"));
  EXPECT_EQ(mi::read_pressure(0xFF, 1)->tx, to_bytes("#FFDS IG\r"));
}

TEST(MicroIonCodec, OutOfRangeArgumentsAreConfigErrors) {
  for (int channel : {0, 4, -1}) {
    auto c = mi::read_pressure(1, channel);
    ASSERT_FALSE(c) << channel;
    EXPECT_EQ(c.error().kind, ErrorKind::Config);
  }
  for (int address : {-1, 256}) {
    auto c = mi::read_pressure(address, 1);
    ASSERT_FALSE(c) << address;
    EXPECT_EQ(c.error().kind, ErrorKind::Config);
    auto s = mi::set_ion_gauge(address, true);
    ASSERT_FALSE(s) << address;
    EXPECT_EQ(s.error().kind, ErrorKind::Config);
  }
}

TEST(MicroIonCodec, SetIonGauge) {
  EXPECT_EQ(*mi::set_ion_gauge(1, true), codec::Command::ascii("#01IG1 ON\r", kLine));
  EXPECT_EQ(*mi::set_ion_gauge(5, false), codec::Command::ascii("#05IG1 OFF\r", kLine));
}

TEST(MicroIonCodec, SensorNames) {
  EXPECT_EQ(mi::to_string(mi::Sensor::IonGauge), "IG");
  EXPECT_EQ(mi::to_string(mi::Sensor::ConvectronA), "CG1");
  EXPECT_EQ(mi::to_string(mi::Sensor::ConvectronB), "CG2");
}

// --- decode -------------------------------------------------------------------

TEST(MicroIonCodec, DecodesPressure) {
  EXPECT_DOUBLE_EQ(*mi::decode_pressure(1, to_bytes("*01 1.20E-06\r")), 1.2e-6);
  EXPECT_DOUBLE_EQ(*mi::decode_pressure(1, to_bytes("*01 7.60E+02\r")), 760.0);
  EXPECT_DOUBLE_EQ(*mi::decode_pressure(0x2A, to_bytes("*2A 3.45E-10\r")), 3.45e-10);
  EXPECT_DOUBLE_EQ(*mi::decode_pressure(0x2A, to_bytes("*2a 3.45E-10\r")), 3.45e-10);  // either case
}

TEST(MicroIonCodec, ToleratesLeadingLineFeedFromCrLfFirmware) {
  EXPECT_DOUBLE_EQ(*mi::decode_pressure(1, to_bytes("\n*01 1.20E-06\r")), 1.2e-6);
  EXPECT_TRUE(mi::decode_ok(1, to_bytes("\n*01 PROGM OK\r")));
}

TEST(MicroIonCodec, GaugeOffIsProtocolError) {
  auto p = mi::decode_pressure(1, to_bytes("*01 9.90E+09\r"));
  ASSERT_FALSE(p);
  expect_protocol(p.error(), "gauge off");
}

TEST(MicroIonCodec, ErrorReplyIsProtocolError) {
  auto p = mi::decode_pressure(1, to_bytes("?01 SYNTX ER\r"));
  ASSERT_FALSE(p);
  expect_protocol(p.error(), "SYNTX ER");

  auto ok = mi::decode_ok(1, to_bytes("?01 INVALID\r"));
  ASSERT_FALSE(ok);
  expect_protocol(ok.error(), "INVALID");
}

TEST(MicroIonCodec, ReplyFromAnotherAddressIsProtocolError) {
  auto p = mi::decode_pressure(1, to_bytes("*02 1.20E-06\r"));
  ASSERT_FALSE(p);
  expect_protocol(p.error(), "address");
}

TEST(MicroIonCodec, MalformedRepliesAreProtocolErrors) {
  for (const char* reply : {
           "*01 1.20E-06",      // no terminator
           "\r",                // empty
           "*01\r",             // no value
           "*01 \r",            // empty value
           "*011.20E-06\r",     // missing separator
           "*01 1.2OE-06\r",    // letter O
           "*01 1.20E-06 X\r",  // trailing junk
           "*0 1.20E-06\r",     // short address
           "!01 1.20E-06\r",    // unknown lead
           "*01 nan\r",
           "*01 -1.00E-06\r",   // negative pressure
       }) {
    auto p = mi::decode_pressure(1, to_bytes(reply));
    ASSERT_FALSE(p) << reply;
    expect_protocol(p.error());
  }
}

TEST(MicroIonCodec, DecodeOk) {
  EXPECT_TRUE(mi::decode_ok(1, to_bytes("*01 PROGM OK\r")));
  EXPECT_FALSE(mi::decode_ok(1, to_bytes("*01 1.20E-06\r")));
  EXPECT_FALSE(mi::decode_ok(2, to_bytes("*01 PROGM OK\r")));
  EXPECT_FALSE(mi::decode_ok(1, to_bytes("*01 PROGM OK")));
}

// --- controller side ------------------------------------------------------------

TEST(MicroIonCodec, DecodesRequests) {
  using K = mi::Request::Kind;
  EXPECT_EQ(*mi::decode_request(to_bytes("#01DS IG\r")), (mi::Request{K::Pressure, 1, 1}));
  EXPECT_EQ(*mi::decode_request(to_bytes("#2ADS CG1\r")), (mi::Request{K::Pressure, 0x2A, 2}));
  EXPECT_EQ(*mi::decode_request(to_bytes("#01DS CG2\r")), (mi::Request{K::Pressure, 1, 3}));
  EXPECT_EQ(*mi::decode_request(to_bytes("#01IG1 ON\r")), (mi::Request{K::IonGaugeOn, 1, 0}));
  EXPECT_EQ(*mi::decode_request(to_bytes("#01IG1 OFF\r")), (mi::Request{K::IonGaugeOff, 1, 0}));
}

TEST(MicroIonCodec, RejectsBadRequests) {
  for (const char* tx : {"#01DS IG", "#01DS CG3\r", "#01XX\r", "01DS IG\r", "#0GDS IG\r", "\r", ""}) {
    auto r = mi::decode_request(to_bytes(tx));
    ASSERT_FALSE(r) << tx;
    expect_protocol(r.error());
  }
}

TEST(MicroIonCodec, AddresseeOfUnreadableCommand) {
  EXPECT_EQ(mi::addressee(to_bytes("#2AXX\r")), 0x2A);
  EXPECT_EQ(mi::addressee(to_bytes("#01")), 1);
  EXPECT_EQ(mi::addressee(to_bytes("#0G")), std::nullopt);
  EXPECT_EQ(mi::addressee(to_bytes("*01 PROGM OK\r")), std::nullopt);
  EXPECT_EQ(mi::addressee(Bytes{}), std::nullopt);
}

TEST(MicroIonCodec, EncodesReplies) {
  EXPECT_EQ(mi::encode_pressure(1, 1.2e-6), to_bytes("*01 1.20E-06\r"));
  EXPECT_EQ(mi::encode_pressure(0xFF, 760.0), to_bytes("*FF 7.60E+02\r"));
  EXPECT_EQ(mi::encode_pressure(1, mi::kGaugeOff), to_bytes("*01 9.90E+09\r"));
  EXPECT_EQ(mi::encode_ok(1), to_bytes("*01 PROGM OK\r"));
  EXPECT_EQ(mi::encode_error(1, "SYNTX ER"), to_bytes("?01 SYNTX ER\r"));
}

TEST(MicroIonCodec, EncodeDecodeRoundTrip) {
  for (double v : {1e-11, 2.5e-9, 1.23e-6, 7.6e2}) {
    auto p = mi::decode_pressure(3, mi::encode_pressure(3, v));
    ASSERT_TRUE(p) << v;
    EXPECT_NEAR(*p, v, v * 0.01);
  }
}
