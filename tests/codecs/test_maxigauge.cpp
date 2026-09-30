// Pfeiffer MaxiGauge (TPG 256 A) codec. Byte strings follow the vendor
// manual's mnemonic protocol: host sends "<MNEMONIC>\r\n", gauge answers
// ACK "\x06\r\n" (or NAK "\x15\r\n"); host then sends ENQ "\x05" and the
// gauge answers with the data line.

#include "pychron/codecs/maxigauge.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace mg = pychron::codec::maxigauge;

namespace {

const ReadSpec kLine = ReadSpec::until("\r\n");

void expect_protocol(const Result<void>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_TRUE(r.error().device.empty());
}

template <class T>
void expect_protocol(const Result<T>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_TRUE(r.error().device.empty());
}

}  // namespace

// --- host commands --------------------------------------------------------

TEST(MaxiGaugeEncode, ReadPressurePerChannel) {
  for (int ch = mg::kFirstChannel; ch <= mg::kLastChannel; ++ch) {
    auto c = mg::read_pressure(ch);
    ASSERT_TRUE(c) << ch;
    EXPECT_EQ(c->tx, to_bytes("PR" + std::to_string(ch) + "\r\n"));
    EXPECT_EQ(c->reply, kLine);
  }
}

TEST(MaxiGaugeEncode, ReadPressureRejectsChannelOutOfRange) {
  for (int ch : {0, 7, -1}) {
    auto c = mg::read_pressure(ch);
    ASSERT_FALSE(c) << ch;
    EXPECT_EQ(c.error().kind, ErrorKind::Config);
  }
}

TEST(MaxiGaugeEncode, OtherMnemonics) {
  EXPECT_EQ(mg::read_all_pressures(), codec::Command::ascii("PRX\r\n", kLine));
  EXPECT_EQ(mg::read_units(), codec::Command::ascii("UNI\r\n", kLine));
}

TEST(MaxiGaugeEncode, EnquiryIsSingleEnqByte) {
  auto c = mg::enquiry();
  EXPECT_EQ(c.tx, (Bytes{0x05}));
  EXPECT_EQ(c.reply, kLine);
}

// --- acknowledgement ------------------------------------------------------

TEST(MaxiGaugeDecodeAck, AckAccepted) { EXPECT_TRUE(mg::decode_ack(to_bytes("\x06\r\n"))); }

TEST(MaxiGaugeDecodeAck, NakIsProtocolError) {
  auto r = mg::decode_ack(to_bytes("\x15\r\n"));
  expect_protocol(r);
  EXPECT_NE(r.error().what.find("NAK"), std::string::npos) << r.error().what;
}

TEST(MaxiGaugeDecodeAck, GarbageTruncatedAndEmptyRejected) {
  expect_protocol(mg::decode_ack(to_bytes("\x06")));      // no terminator
  expect_protocol(mg::decode_ack(to_bytes("OK\r\n")));
  expect_protocol(mg::decode_ack(to_bytes("\x06\x06\r\n")));
  expect_protocol(mg::decode_ack(Bytes{}));
}

// --- single reading -------------------------------------------------------

TEST(MaxiGaugeDecodeReading, OkReading) {
  auto r = mg::decode_reading(to_bytes("0,+1.2300E-08\r\n"));
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->status, mg::Status::Ok);
  EXPECT_DOUBLE_EQ(r->value, 1.23e-8);
}

TEST(MaxiGaugeDecodeReading, AllStatusCodes) {
  const mg::Status expected[] = {mg::Status::Ok,         mg::Status::Underrange, mg::Status::Overrange,
                                 mg::Status::SensorError, mg::Status::SensorOff, mg::Status::NoSensor,
                                 mg::Status::IdentificationError};
  for (int s = 0; s <= 6; ++s) {
    auto r = mg::decode_reading(to_bytes(std::to_string(s) + ",+1.0000E-03\r\n"));
    ASSERT_TRUE(r) << s;
    EXPECT_EQ(r->status, expected[s]);
  }
}

TEST(MaxiGaugeDecodeReading, BoundaryValues) {
  EXPECT_DOUBLE_EQ(mg::decode_reading(to_bytes("0,+1.0000E+03\r\n"))->value, 1000.0);
  EXPECT_DOUBLE_EQ(mg::decode_reading(to_bytes("1,+5.0000E-11\r\n"))->value, 5e-11);
  EXPECT_DOUBLE_EQ(mg::decode_reading(to_bytes("0,-2.5000E-01\r\n"))->value, -0.25);
  EXPECT_DOUBLE_EQ(mg::decode_reading(to_bytes("0,+0.0000E+00\r\n"))->value, 0.0);
  EXPECT_DOUBLE_EQ(mg::decode_reading(to_bytes("0,1.5E-6\r\n"))->value, 1.5e-6);
}

TEST(MaxiGaugeDecodeReading, MalformedFramesRejected) {
  for (const char* bad : {
           "0,+1.2300E-08",        // truncated: no terminator
           "0,+1.2300E-08\r",      // half terminator
           "\r\n",                 // empty line
           "0\r\n",                // no value
           "0,\r\n",               // empty value
           ",+1.0E-08\r\n",        // empty status
           "7,+1.0E-08\r\n",       // unknown status
           "a,+1.0E-08\r\n",       // non-digit status
           "00,+1.0E-08\r\n",      // status is one digit
           "0,+1.0E-0x\r\n",       // unparsable number
           "0,+1.0E-08 \r\n",      // trailing junk
           "0,1.0,2.0\r\n",        // too many fields
           "0,nan\r\n",            // not a number the gauge sends
           "0,inf\r\n",
           "\x15\r\n",             // NAK where data was expected
       }) {
    expect_protocol(mg::decode_reading(to_bytes(bad)));
  }
}

TEST(MaxiGaugeDecodeReading, ErrorCarriesEscapedReply) {
  auto r = mg::decode_reading(to_bytes("9,x\r\n"));
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("9,x\\r\\n"), std::string::npos) << r.error().what;
}

// --- pressure -------------------------------------------------------------

TEST(MaxiGaugeDecodePressure, OkAndRangeLimitsYieldValue) {
  EXPECT_DOUBLE_EQ(*mg::decode_pressure(to_bytes("0,+2.0000E-07\r\n")), 2e-7);
  // Under/overrange: the gauge reports its range limit; the value is usable.
  EXPECT_DOUBLE_EQ(*mg::decode_pressure(to_bytes("1,+1.0000E-11\r\n")), 1e-11);
  EXPECT_DOUBLE_EQ(*mg::decode_pressure(to_bytes("2,+1.0000E+03\r\n")), 1e3);
}

TEST(MaxiGaugeDecodePressure, SensorFaultsAreProtocolErrors) {
  for (const char* s : {"3", "4", "5", "6"}) {
    auto r = mg::decode_pressure(to_bytes(std::string(s) + ",+0.0000E+00\r\n"));
    expect_protocol(r);
  }
  auto off = mg::decode_pressure(to_bytes("4,+0.0000E+00\r\n"));
  EXPECT_NE(off.error().what.find("sensor off"), std::string::npos) << off.error().what;
}

TEST(MaxiGaugeDecodePressure, PressureValueOfReading) {
  EXPECT_DOUBLE_EQ(*mg::pressure_value({mg::Status::Ok, 3.0}), 3.0);
  expect_protocol(mg::pressure_value({mg::Status::NoSensor, 0.0}));
}

// --- all channels ---------------------------------------------------------

TEST(MaxiGaugeDecodeAll, SixReadings) {
  auto r = mg::decode_all_readings(to_bytes(
      "0,+1.0000E-08,0,+2.0000E-07,1,+1.0000E-11,5,+0.0000E+00,4,+0.0000E+00,2,+1.0000E+03\r\n"));
  ASSERT_TRUE(r) << to_string(r.error());
  ASSERT_EQ(r->size(), 6u);
  EXPECT_EQ((*r)[0], (mg::Reading{mg::Status::Ok, 1e-8}));
  EXPECT_EQ((*r)[1], (mg::Reading{mg::Status::Ok, 2e-7}));
  EXPECT_EQ((*r)[2].status, mg::Status::Underrange);
  EXPECT_EQ((*r)[3].status, mg::Status::NoSensor);
  EXPECT_EQ((*r)[4].status, mg::Status::SensorOff);
  EXPECT_EQ((*r)[5], (mg::Reading{mg::Status::Overrange, 1e3}));
}

TEST(MaxiGaugeDecodeAll, WrongFieldCountRejected) {
  expect_protocol(mg::decode_all_readings(to_bytes("0,+1.0000E-08\r\n")));
  expect_protocol(mg::decode_all_readings(to_bytes(
      "0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0,+1.0E-08\r\n")));
  expect_protocol(mg::decode_all_readings(to_bytes(
      "0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0\r\n")));
  expect_protocol(mg::decode_all_readings(to_bytes(
      "0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0,+1.0E-08,0,+1.0E-08")));
}

// --- units ----------------------------------------------------------------

TEST(MaxiGaugeDecodeUnits, KnownUnits) {
  EXPECT_EQ(*mg::decode_units(to_bytes("0\r\n")), mg::Units::Mbar);
  EXPECT_EQ(*mg::decode_units(to_bytes("1\r\n")), mg::Units::Torr);
  EXPECT_EQ(*mg::decode_units(to_bytes("2\r\n")), mg::Units::Pascal);
  EXPECT_EQ(mg::to_string(mg::Units::Torr), "torr");
}

TEST(MaxiGaugeDecodeUnits, UnknownRejected) {
  for (const char* bad : {"3\r\n", "\r\n", "1", "10\r\n", "x\r\n"}) {
    expect_protocol(mg::decode_units(to_bytes(bad)));
  }
}

// --- gauge side (simulation) ---------------------------------------------

TEST(MaxiGaugeGaugeSide, EncodedRepliesRoundTrip) {
  EXPECT_EQ(mg::encode_ack(), to_bytes("\x06\r\n"));
  EXPECT_EQ(mg::encode_nak(), to_bytes("\x15\r\n"));
  EXPECT_EQ(mg::encode_reading({mg::Status::Ok, 1.23e-8}), to_bytes("0,+1.2300E-08\r\n"));
  EXPECT_EQ(mg::encode_reading({mg::Status::SensorOff, 0.0}), to_bytes("4,+0.0000E+00\r\n"));
  EXPECT_EQ(mg::encode_units(mg::Units::Pascal), to_bytes("2\r\n"));

  std::vector<mg::Reading> six(6, mg::Reading{mg::Status::NoSensor, 0.0});
  six[0] = {mg::Status::Ok, 4.5e-9};
  auto all = mg::encode_all_readings(six);
  auto back = mg::decode_all_readings(all);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, six);
  EXPECT_DOUBLE_EQ(mg::decode_reading(mg::encode_reading({mg::Status::Ok, 7.25e-5}))->value, 7.25e-5);
}

TEST(MaxiGaugeGaugeSide, DecodeRequest) {
  auto pr = mg::decode_request(to_bytes("PR3\r\n"));
  ASSERT_TRUE(pr);
  EXPECT_EQ(pr->kind, mg::Request::Kind::Pressure);
  EXPECT_EQ(pr->channel, 3);

  EXPECT_EQ(mg::decode_request(to_bytes("PRX\r\n"))->kind, mg::Request::Kind::AllPressures);
  EXPECT_EQ(mg::decode_request(to_bytes("UNI\r\n"))->kind, mg::Request::Kind::Units);
  EXPECT_EQ(mg::decode_request(Bytes{0x05})->kind, mg::Request::Kind::Enquiry);
  // The gauge also accepts a bare CR terminator.
  EXPECT_EQ(mg::decode_request(to_bytes("PR1\r"))->channel, 1);

  for (const char* bad : {"PR7\r\n", "PR0\r\n", "PR\r\n", "XYZ\r\n", "PR1", ""}) {
    expect_protocol(mg::decode_request(to_bytes(bad)));
  }
}

TEST(MaxiGaugeGaugeSide, RequestsMatchHostEncoding) {
  EXPECT_EQ(mg::decode_request(mg::read_pressure(6)->tx)->channel, 6);
  EXPECT_EQ(mg::decode_request(mg::read_all_pressures().tx)->kind, mg::Request::Kind::AllPressures);
  EXPECT_EQ(mg::decode_request(mg::read_units().tx)->kind, mg::Request::Kind::Units);
  EXPECT_EQ(mg::decode_request(mg::enquiry().tx)->kind, mg::Request::Kind::Enquiry);
}
