#include "pychron/codecs/isotopx_ngx.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace ngx = pychron::codec::ngx;

namespace {
const ReadSpec kLine = ReadSpec::until("#\n");
Bytes B(std::string_view s) { return to_bytes(s); }
}  // namespace

TEST(NgxCodec, Encoding) {
  EXPECT_EQ(*ngx::login("admin", "pw"), codec::Command::ascii("Login admin,pw#\n", kLine));
  EXPECT_EQ(*ngx::set_mass(39.9624), codec::Command::ascii("SetMass 39.9624#\n", kLine));
  EXPECT_EQ(ngx::get_mass(), codec::Command::ascii("GetMass#\n", kLine));
  EXPECT_EQ(*ngx::start_acq(), codec::Command::ascii("StartAcq 1#\n", kLine));
  EXPECT_EQ(ngx::stop_acq(), codec::Command::ascii("StopAcq#\n", kLine));
  EXPECT_EQ(*ngx::set_acq_period(2), codec::Command::ascii("SetAcqPeriod 2#\n", kLine));
  EXPECT_EQ(ngx::sab(), codec::Command::ascii("SAB#\n", kLine));
  EXPECT_EQ(*ngx::set_source_param(ngx::Param::YSymmetry, 1.5), codec::Command::ascii("SSO YF,1.5#\n", kLine));
  EXPECT_EQ(*ngx::get_source_param(ngx::Param::YSymmetry), codec::Command::ascii("GSO YF#\n", kLine));
  EXPECT_EQ(*ngx::get_source_param("XTRA.1"), codec::Command::ascii("GSO XTRA.1#\n", kLine));
}

TEST(NgxCodec, InvalidArgsAreConfig) {
  EXPECT_EQ(ngx::login("a,b", "x").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_mass(-1).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_acq_period(0).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::start_acq(0).error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::get_source_param("bad name").error().kind, ErrorKind::Config);
  EXPECT_EQ(ngx::set_source_param("A", NAN).error().kind, ErrorKind::Config);
}

TEST(NgxCodec, MnemonicMapRoundTrips) {
  for (int i = 0; i <= static_cast<int>(ngx::Param::ESAMinus); ++i) {
    auto p = static_cast<ngx::Param>(i);
    auto m = ngx::mnemonic(p);
    ASSERT_TRUE(m) << i;
    EXPECT_EQ(ngx::param_from_mnemonic(*m), p);
  }
  EXPECT_FALSE(ngx::param_from_mnemonic("nope"));
}

TEST(NgxCodec, Replies) {
  EXPECT_TRUE(ngx::decode_ok(B("OK#\n")));
  EXPECT_DOUBLE_EQ(*ngx::decode_mass(B("OK,39.9624#\n")), 39.9624);
  auto rb = *ngx::decode_source_param(B("OK,1.5,1.48#\n"));
  EXPECT_DOUBLE_EQ(rb.setpoint, 1.5);
  EXPECT_DOUBLE_EQ(*rb.actual, 1.48);
  auto sp_only = *ngx::decode_source_param(B("OK,2#\n"));
  EXPECT_FALSE(sp_only.actual);
}

TEST(NgxCodec, BadReplies) {
  for (auto s : {"OK", "OK,abc#\n", "WAT#\n", ""}) {
    auto r = ngx::decode_mass(B(s));
    ASSERT_FALSE(r) << s;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  }
  EXPECT_FALSE(ngx::decode_mass(B("OK#\n")));
  EXPECT_FALSE(ngx::decode_source_param(B("OK,1,2,3#\n")));
}

TEST(NgxCodec, ErrorCodeMapping) {
  auto kind = [](const char* s) { return ngx::decode_ok(B(s)).error().kind; };
  EXPECT_EQ(kind("E01,unknown#\n"), ErrorKind::Protocol);
  EXPECT_EQ(kind("E02#\n"), ErrorKind::Config);
  EXPECT_EQ(kind("E04#\n"), ErrorKind::NotConnected);
  EXPECT_EQ(kind("E05#\n"), ErrorKind::Config);
  EXPECT_EQ(kind("E07#\n"), ErrorKind::Interlock);
  EXPECT_EQ(kind("E08#\n"), ErrorKind::Timeout);
  EXPECT_EQ(kind("E99#\n"), ErrorKind::Protocol);
  EXPECT_NE(ngx::decode_ok(B("E03,busy#\n")).error().what.find("busy"), std::string::npos);
}

TEST(NgxCodec, AcqEventReversesDetectorOrder) {
  auto f = *ngx::decode_acq_event(B("#EVENT:ACQ,7,12.5,0.3,0.2,0.1#\n"));
  EXPECT_EQ(f.seq, 7u);
  EXPECT_DOUBLE_EQ(f.ts_s, 12.5);
  EXPECT_FALSE(f.baseline);
  EXPECT_EQ(f.values, (std::vector<double>{0.1, 0.2, 0.3}));
  auto b = *ngx::decode_acq_event(B("#EVENT:ACQ.B,8,13.5,1e-3,2#\n"));
  EXPECT_TRUE(b.baseline);
  EXPECT_EQ(b.values, (std::vector<double>{2, 1e-3}));
}

TEST(NgxCodec, AcqEventGarbage) {
  for (auto s : {"#EVENT:ACQ,1,2#\n", "#EVENT:ACQ,x,2,3#\n", "#EVENT:ACQ,1,2,zz#\n", "#EVENT:ACQ,-1,2,3#\n",
                 "#EVENT:FOO,1,2,3#\n", "OK#\n", "#EVENT:ACQ,1,2,3"}) {
    auto r = ngx::decode_acq_event(B(s));
    ASSERT_FALSE(r) << s;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  }
}

TEST(NgxDemux, SeparatesRepliesFromEventsAcrossChunks) {
  ngx::Demultiplexer d;
  EXPECT_TRUE(d.feed(B("OK,39.9")).empty());
  auto out = d.feed(B("624#\n#EVENT:ACQ,1,0.5,2,1#\n#EVENT:STATE,idle#\n#EVENT:ACQ,2,1"));
  ASSERT_EQ(out.size(), 3u);
  ASSERT_TRUE(out[0]);
  EXPECT_EQ(std::get<ngx::Reply>(*out[0]).raw, B("OK,39.9624#\n"));
  EXPECT_EQ(std::get<ngx::AcqFrame>(*out[1]).values, (std::vector<double>{1, 2}));
  EXPECT_EQ(std::get<ngx::OtherEvent>(*out[2]).name, "STATE");
  EXPECT_EQ(std::get<ngx::OtherEvent>(*out[2]).body, "idle");
  EXPECT_GT(d.pending(), 0u);
  out = d.feed(B(".5,4#\nOK#\n"));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(std::get<ngx::AcqFrame>(*out[0]).seq, 2u);
  EXPECT_TRUE(std::holds_alternative<ngx::Reply>(*out[1]));
  EXPECT_EQ(d.pending(), 0u);
}

TEST(NgxDemux, MalformedEventDoesNotBlockLaterLines) {
  ngx::Demultiplexer d;
  auto out = d.feed(B("#EVENT:ACQ,bad#\nOK#\n"));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_FALSE(out[0]);
  EXPECT_TRUE(out[1]);
}
