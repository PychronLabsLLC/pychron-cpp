#include "pychron/codecs/isotopx_ngx.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace pychron;
namespace ngx = pychron::codec::ngx;

// Wire literals mirror pychron's Python NGX driver (pychron/hardware/
// isotopx_spectrometer_controller.py, pychron/spectrometer/isotopx/*).

namespace {
const ReadSpec kLine = ReadSpec::until("#\r\n");
Bytes B(std::string_view s) { return to_bytes(s); }
codec::Command C(std::string_view s) { return codec::Command::ascii(s, kLine); }
}  // namespace

// --- framing ------------------------------------------------------------------

TEST(NgxCodec, FramingIsHashCrLf) {
  EXPECT_EQ(ngx::kTerminator, "#\r\n");
  EXPECT_EQ(ngx::kDefaultSendTerminator, "#\r\n");
  EXPECT_EQ(ngx::stop_acq(), C("StopAcq#\r\n"));
}

TEST(NgxCodec, SendTerminatorIsAParameter) {
  EXPECT_EQ(ngx::stop_acq("\r"), C("StopAcq\r"));
  EXPECT_EQ(ngx::get_mass("\r\n"), C("GETMASS\r\n"));
  EXPECT_EQ(*ngx::login("admin", "pw", "\r"), C("Login admin,pw\r"));
  // reply framing does not follow the send terminator
  EXPECT_EQ(*ngx::set_mass(40.0, 500, false, "\r")->reply, kLine);
}

// --- encoders -----------------------------------------------------------------

TEST(NgxCodec, Encoding) {
  EXPECT_EQ(*ngx::login("admin", "pw"), C("Login admin,pw#\r\n"));
  EXPECT_EQ(ngx::get_mass(), C("GETMASS#\r\n"));
  EXPECT_EQ(*ngx::set_mass(39.9624, 500), C("SetMass 39.9624,500#\r\n"));
  EXPECT_EQ(*ngx::set_mass(39.9624, 500, true), C("SetMass 39.9624,500,deflect#\r\n"));
  EXPECT_EQ(*ngx::start_acq(10), C("StartAcq 10,NOM#\r\n"));
  EXPECT_EQ(*ngx::start_acq(4.9, "RCS2"), C("StartAcq 4,RCS2#\r\n"));  // int() truncates
  EXPECT_EQ(ngx::stop_acq(), C("StopAcq#\r\n"));
  EXPECT_EQ(*ngx::set_acq_period(1000), C("SetAcqPeriod 1000#\r\n"));
  EXPECT_EQ(ngx::sab(true), C("SAB 1#\r\n"));
  EXPECT_EQ(ngx::sab(false), C("SAB 0#\r\n"));
}

TEST(NgxCodec, SourceParamEncoding) {
  EXPECT_EQ(*ngx::set_source_param(ngx::Param::IonEnergy, 4500.0), C("SSO IE, 4500.0#\r\n"));
  EXPECT_EQ(*ngx::set_source_param(ngx::Param::YFocus, 1.5), C("SSO YF, 1.5#\r\n"));
  EXPECT_EQ(*ngx::get_source_param(ngx::Param::IonEnergy), C("GSO IE#\r\n"));
  EXPECT_EQ(*ngx::get_source_param(ngx::Param::ESAPlus), C("GSO ESA+#\r\n"));
  EXPECT_EQ(*ngx::get_source_param("XTRA.1"), C("GSO XTRA.1#\r\n"));
  EXPECT_EQ(*ngx::set_source_output("YF", 1.25), C("SetSourceOutput YF,1.25#\r\n"));
  EXPECT_EQ(*ngx::get_source_output("TC"), C("GetSourceOutput TC#\r\n"));
  EXPECT_EQ(*ngx::get_source_output(ngx::Param::ESAMinus), C("GetSourceOutput ESA-#\r\n"));
}

TEST(NgxCodec, ValveEncoding) {
  EXPECT_EQ(*ngx::open_valve("3"), C("OpenValve 3#\r\n"));
  EXPECT_EQ(*ngx::close_valve("3"), C("CloseValve 3#\r\n"));
  EXPECT_EQ(*ngx::get_valve_status("12"), C("GetValveStatus 12#\r\n"));
}

TEST(NgxCodec, SettlingDelayTruncatesLikePythonInt) {
  EXPECT_EQ(*ngx::settling_delay_ms(0.5), 500);
  EXPECT_EQ(*ngx::settling_delay_ms(0.0019), 1);
  EXPECT_EQ(ngx::settling_delay_ms(-1).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::settling_delay_ms(NAN).error().kind, ErrorKind::Config);
}

TEST(NgxCodec, FloatsFormatLikePythonStr) {
  EXPECT_EQ(ngx::format_float(1.5), "1.5");
  EXPECT_EQ(ngx::format_float(4500.0), "4500.0");
  EXPECT_EQ(ngx::format_float(0.1), "0.1");
  EXPECT_EQ(ngx::format_float(39.962383123), "39.962383123");
  EXPECT_EQ(ngx::format_float(100000.0), "100000.0");
  EXPECT_EQ(ngx::format_float(1e-5), "1e-05");
  EXPECT_EQ(ngx::format_float(0.0001), "0.0001");
  EXPECT_EQ(ngx::format_float(1.5e16), "1.5e+16");
  EXPECT_EQ(ngx::format_float(1e22), "1e+22");
  EXPECT_EQ(ngx::format_float(-2.25), "-2.25");
  EXPECT_EQ(ngx::format_float(0.0), "0.0");
  EXPECT_EQ(ngx::format_float(1.0 / 3.0), "0.3333333333333333");
  EXPECT_EQ(*ngx::set_mass(1.0 / 3.0, 0), C("SetMass 0.3333333333333333,0#\r\n"));
}

TEST(NgxCodec, InvalidArgsAreConfig) {
  EXPECT_EQ(ngx::login("a,b", "x").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_mass(-1, 0).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_mass(NAN, 0).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_mass(40, -1).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_acq_period(0).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::start_acq(0.5).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::start_acq(NAN).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::start_acq(1, "a,b").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::start_acq(1, "").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::get_source_param("bad name").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_source_param("A", NAN).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_source_output("A,B", 1).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::get_source_output("").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::open_valve("").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::close_valve("1 2").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::get_valve_status("1#").error().kind, ErrorKind::Config);
}

TEST(NgxCodec, MnemonicMapIsPythons) {
  const std::pair<ngx::Param, std::string_view> expected[] = {
      {ngx::Param::IonEnergy, "IE"},        {ngx::Param::YFocus, "YF"},
      {ngx::Param::YBias, "YB"},            {ngx::Param::ZFocus, "ZF"},
      {ngx::Param::ZBias, "ZB"},            {ngx::Param::ElectronEnergy, "EE"},
      {ngx::Param::IonRepeller, "IR"},      {ngx::Param::TrapVoltage, "TV"},
      {ngx::Param::FilamentCurrent, "FC"},  {ngx::Param::FilamentVoltage, "FV"},
      {ngx::Param::TrapCurrent, "TC"},      {ngx::Param::EmissionCurrent, "EC"},
      {ngx::Param::ConfinementVoltage, "CV"}, {ngx::Param::ESAPlus, "ESA+"},
      {ngx::Param::ESAMinus, "ESA-"},
  };
  for (auto& [p, m] : expected) {
    EXPECT_EQ(ngx::mnemonic(p), m);
    EXPECT_EQ(ngx::param_from_mnemonic(m), p);
  }
  EXPECT_EQ(ngx::param_from_name("ESA+Plate"), ngx::Param::ESAPlus);
  EXPECT_EQ(ngx::param_from_name("IonEnergy"), ngx::Param::IonEnergy);
  EXPECT_EQ(ngx::name(ngx::Param::EmissionCurrent), "EmissionCurrent");
  for (auto gone : {"HV", "EM", "EL", "EF", "ES", "ZS", "HS", "FP", "RQ", "PN", "PS", "E+", "E-"})
    EXPECT_FALSE(ngx::param_from_mnemonic(gone)) << gone;
  EXPECT_FALSE(ngx::param_from_mnemonic("nope"));
}

// --- replies ------------------------------------------------------------------

TEST(NgxCodec, SuccessIsE00) {
  EXPECT_TRUE(ngx::decode_ok(B("E00#\r\n")));
  auto ok = ngx::decode_ok(B("OK#\r\n"));
  ASSERT_FALSE(ok);
  EXPECT_EQ(ok.error().kind, ErrorKind::Protocol);
}

TEST(NgxCodec, BareValueReplies) {
  EXPECT_DOUBLE_EQ(*ngx::decode_mass(B("39.9624#\r\n")), 39.9624);
  auto rb = *ngx::decode_source_param(B("1.5,1.48#\r\n"));
  EXPECT_DOUBLE_EQ(rb.setpoint, 1.5);
  EXPECT_DOUBLE_EQ(rb.actual, 1.48);
  auto spaced = *ngx::decode_source_param(B("4500, 4499.8#\r\n"));
  EXPECT_DOUBLE_EQ(spaced.actual, 4499.8);
}

TEST(NgxCodec, BadReplies) {
  for (auto s : {"39.9", "39.9#\n", "abc#\r\n", "OK,39.9#\r\n", "#\r\n", ""}) {
    auto r = ngx::decode_mass(B(s));
    ASSERT_FALSE(r) << s;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << s;
  }
  for (auto s : {"1.5#\r\n", "1,2,3#\r\n", "OK,1,2#\r\n", "1,x#\r\n"}) {
    auto r = ngx::decode_source_param(B(s));
    ASSERT_FALSE(r) << s;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << s;
  }
}

TEST(NgxCodec, ValveReplies) {
  EXPECT_TRUE(ngx::decode_ok(B("E00#\r\n")));
  EXPECT_TRUE(*ngx::decode_valve_status(B("OPEN#\r\n")));
  EXPECT_FALSE(*ngx::decode_valve_status(B("CLOSED#\r\n")));
  // Python's actuator re-queries when E00 arrives in place of a status.
  auto ack = ngx::decode_valve_status(B("E00#\r\n"));
  ASSERT_FALSE(ack);
  EXPECT_EQ(ack.error().kind, ErrorKind::Protocol);
  EXPECT_FALSE(ngx::decode_valve_status(B("open#\r\n")));
  EXPECT_EQ(ngx::decode_valve_status(B("E31#\r\n")).error().kind, ErrorKind::Io);
}

TEST(NgxCodec, ErrorCodeTableIsPythons) {
  struct Row { const char* wire; ErrorKind kind; const char* name; };
  const Row rows[] = {
      {"E01", ErrorKind::Protocol, "ERR_INVALID_COMMAND"},
      {"E02", ErrorKind::Config, "ERR_INVALID_PARAM"},
      {"E03", ErrorKind::Config, "ERR_OUT_OF_RANGE"},
      {"E04", ErrorKind::Config, "ERR_INVALID_MNEMONIC"},
      {"E05", ErrorKind::Config, "ERR_MISSING_PARAMS"},
      {"E20", ErrorKind::Cancelled, "ERR_ABORTEDBY_SYSTEM"},
      {"E21", ErrorKind::Cancelled, "ERR_ABORTEDBY_USER"},
      {"E30", ErrorKind::Io, "ERR_HARDWARE_MISSING"},
      {"E31", ErrorKind::Io, "ERR_HARDWARE_FAULT"},
      {"E32", ErrorKind::Timeout, "ERR_TIMEOUT"},
      {"E40", ErrorKind::Protocol, "ERR_AVAILABLE"},
      {"E41", ErrorKind::Io, "ERR_BUSY"},
      {"E42", ErrorKind::NotConnected, "ERR_ACCESS_DENIED"},
      {"E43", ErrorKind::Io, "ERR_NOT_AVAILABLE"},
      {"E44", ErrorKind::Protocol, "ERR_NO_RESULTS_AVAILABLE"},
      {"E45", ErrorKind::Protocol, "ERR_UNIT_NOT_IN_TRIP_STATE"},
      {"E99", ErrorKind::Protocol, "ERR_UNKNOWN"},
  };
  for (auto& r : rows) {
    auto e = ngx::decode_ok(B(std::string(r.wire) + "#\r\n"));
    ASSERT_FALSE(e) << r.wire;
    EXPECT_EQ(e.error().kind, r.kind) << r.wire;
    EXPECT_NE(e.error().what.find(r.wire), std::string::npos) << e.error().what;
    EXPECT_NE(e.error().what.find(r.name), std::string::npos) << e.error().what;
  }
  // errors reach every decoder, with an optional ",text" suffix
  EXPECT_EQ(ngx::decode_mass(B("E32#\r\n")).error().kind, ErrorKind::Timeout);
  auto sfx = ngx::decode_source_param(B("E41,busy acquiring#\r\n"));
  EXPECT_EQ(sfx.error().kind, ErrorKind::Io);
  EXPECT_NE(sfx.error().what.find("busy acquiring"), std::string::npos);
  // unlisted code: still an error, Protocol
  auto unk = ngx::decode_ok(B("E77#\r\n"));
  ASSERT_FALSE(unk);
  EXPECT_EQ(unk.error().kind, ErrorKind::Protocol);
}

// --- ACQ events ---------------------------------------------------------------

TEST(NgxCodec, AcqEventLayout) {
  auto f = *ngx::decode_acq_event(B("#EVENT:ACQ,NOM,SIM,7,12:34:56.789012,0.3,0.2,0.1#\r\n"));
  EXPECT_FALSE(f.baseline);
  EXPECT_EQ(f.rcs_id, "NOM");
  EXPECT_EQ(f.field2, "SIM");
  EXPECT_EQ(f.field3, "7");
  EXPECT_EQ(f.time.hour, 12);
  EXPECT_EQ(f.time.minute, 34);
  EXPECT_EQ(f.time.second, 56);
  EXPECT_EQ(f.time.microsecond, 789012);
  EXPECT_EQ(f.time.microseconds_since_midnight(), ((12LL * 60 + 34) * 60 + 56) * 1000000LL + 789012);
  // wire is reverse detector order; frame is channel order
  EXPECT_EQ(f.values, (std::vector<double>{0.1, 0.2, 0.3}));
}

TEST(NgxCodec, AcqBaselineEventAndLenientFields) {
  // %f takes 1-6 digits (right padded), %H/%M/%S 1-2 digits; values may carry spaces.
  auto b = *ngx::decode_acq_event(B("#EVENT:ACQ.B,NOM,,,1:02:3.5, 1e-3,2#\r\n"));
  EXPECT_TRUE(b.baseline);
  EXPECT_EQ(b.field2, "");
  EXPECT_EQ(b.time.hour, 1);
  EXPECT_EQ(b.time.second, 3);
  EXPECT_EQ(b.time.microsecond, 500000);
  EXPECT_EQ(b.values, (std::vector<double>{2, 1e-3}));
}

TEST(NgxCodec, AcqEventGarbage) {
  for (auto s : {"#EVENT:ACQ,NOM,a,b,12:00:00.0#\r\n",      // < 6 fields
                 "#EVENT:ACQ,NOM,a,b,12:00:00,1#\r\n",      // no .%f
                 "#EVENT:ACQ,NOM,a,b,24:00:00.0,1#\r\n",    // hour range
                 "#EVENT:ACQ,NOM,a,b,12:60:00.0,1#\r\n",    // minute range
                 "#EVENT:ACQ,NOM,a,b,12:00:00.1234567,1#\r\n",
                 "#EVENT:ACQ,NOM,a,b,12:00:00.0,zz#\r\n",
                 "#EVENT:ACQ,NOM,a,b,12:00:00.0,#\r\n",
                 "#EVENT:FOO,NOM,a,b,12:00:00.0,1#\r\n", "E00#\r\n",
                 "#EVENT:ACQ,NOM,a,b,12:00:00.0,1#\n"}) {
    auto r = ngx::decode_acq_event(B(s));
    ASSERT_FALSE(r) << s;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << s;
  }
}

// --- demultiplexer ------------------------------------------------------------

TEST(NgxDemux, SeparatesRepliesFromEventsAcrossChunks) {
  ngx::Demultiplexer d;
  EXPECT_TRUE(d.feed(B("39.9")).empty());
  auto out = d.feed(B("624#\r\n#EVENT:ACQ,NOM,x,y,00:00:01.0,2,1#\r\n#EVENT:STATE,idle#\r\n#EVENT:ACQ,NOM,x,y,00:00:02.0,1"));
  ASSERT_EQ(out.size(), 3u);
  ASSERT_TRUE(out[0]);
  EXPECT_EQ(std::get<ngx::Reply>(*out[0]).raw, B("39.9624#\r\n"));
  EXPECT_EQ(std::get<ngx::AcqFrame>(*out[1]).values, (std::vector<double>{1, 2}));
  EXPECT_EQ(std::get<ngx::OtherEvent>(*out[2]).name, "STATE");
  EXPECT_EQ(std::get<ngx::OtherEvent>(*out[2]).body, "idle");
  EXPECT_GT(d.pending(), 0u);
  out = d.feed(B(".5,4#\r\nE00#\r\n"));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(std::get<ngx::AcqFrame>(*out[0]).time.second, 2);
  EXPECT_TRUE(std::holds_alternative<ngx::Reply>(*out[1]));
  EXPECT_EQ(d.pending(), 0u);
}

TEST(NgxDemux, TerminatorSplitAcrossChunks) {
  ngx::Demultiplexer d;
  EXPECT_TRUE(d.feed(B("E00#")).empty());
  EXPECT_TRUE(d.feed(B("\r")).empty());
  auto out = d.feed(B("\nOPEN#\r"));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(std::get<ngx::Reply>(*out[0]).raw, B("E00#\r\n"));
  out = d.feed(B("\n"));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(std::get<ngx::Reply>(*out[0]).raw, B("OPEN#\r\n"));
  // a bare "#\n" is not a terminator any more
  EXPECT_TRUE(d.feed(B("E00#\n")).empty());
  EXPECT_GT(d.pending(), 0u);
}

TEST(NgxDemux, MalformedEventDoesNotBlockLaterLines) {
  ngx::Demultiplexer d;
  auto out = d.feed(B("#EVENT:ACQ,bad#\r\nE00#\r\n"));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_FALSE(out[0]);
  EXPECT_TRUE(out[1]);
}
