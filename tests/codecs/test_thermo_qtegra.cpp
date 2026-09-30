#include "pychron/codecs/thermo_qtegra.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>

using namespace pychron;
namespace q = pychron::codec::qtegra;

namespace {

const ReadSpec kLine = ReadSpec::until("\n");

codec::Command cmd(std::string_view s) { return codec::Command::ascii(s, kLine); }

template <class T>
void expect_protocol(const Result<T>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol) << to_string(r.error());
  EXPECT_TRUE(r.error().device.empty());
}

template <class T>
void expect_config(const Result<T>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

}  // namespace

TEST(QtegraCodec, EncodesEveryCommand) {
  EXPECT_EQ(*q::set_magnet_dac(4.5), cmd("SetMagnetDAC 4.5\n"));
  EXPECT_EQ(*q::get_magnet_dac(), cmd("GetMagnetDAC\n"));
  EXPECT_EQ(*q::get_magnet_moving(), cmd("GetMagnetMoving\n"));
  EXPECT_EQ(*q::blank_beam(true), cmd("BlankBeam True\n"));
  EXPECT_EQ(*q::blank_beam(false), cmd("BlankBeam False\n"));
  EXPECT_EQ(*q::protect_detector("H1", true), cmd("ProtectDetector H1,On\n"));
  EXPECT_EQ(*q::protect_detector("H1", false), cmd("ProtectDetector H1,Off\n"));
  EXPECT_EQ(*q::set_deflection("CDD", 120), cmd("SetDeflection CDD,120\n"));
  EXPECT_EQ(*q::get_deflection("CDD"), cmd("GetDeflection CDD\n"));
  EXPECT_EQ(*q::get_deflections(), cmd("GetDeflections\n"));
  EXPECT_EQ(*q::set_gain("H1", 1.5), cmd("SetGain H1,1.5\n"));
  EXPECT_EQ(*q::get_gain("H1"), cmd("GetGain H1\n"));
  EXPECT_EQ(*q::set_ion_counter_voltage(1450), cmd("SetIonCounterVoltage 1450\n"));
  EXPECT_EQ(*q::get_integration_time(), cmd("GetIntegrationTime\n"));
  EXPECT_EQ(*q::get_data(), cmd("GetData\n"));
  EXPECT_EQ(*q::set_hv(4.5), cmd("SetHV 4.5\n"));
  EXPECT_EQ(*q::get_high_voltage(), cmd("GetHighVoltage\n"));
  EXPECT_EQ(*q::set_parameter("Y-Symmetry Set", -1.25), cmd("SetParameter Y-Symmetry Set,-1.25\n"));
  EXPECT_EQ(*q::get_parameter("Y-Symmetry Set"), cmd("GetParameter Y-Symmetry Set\n"));
  EXPECT_EQ(*q::get_parameters(), cmd("GetParameters\n"));
  EXPECT_EQ(*q::reset(), cmd("Reset\n"));
}

TEST(QtegraCodec, BadArgumentsAreConfigErrors) {
  expect_config(q::set_magnet_dac(NAN));
  expect_config(q::set_hv(INFINITY));
  expect_config(q::set_gain("", 1));
  expect_config(q::set_deflection("H,1", 1));
  expect_config(q::get_gain("H1\n"));
  expect_config(q::set_integration_time(NAN));
}

TEST(QtegraCodec, IntegrationTimeSnapsToBinarySeries) {
  EXPECT_DOUBLE_EQ(q::snap_integration_time(0.0), 0.065536);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(-3), 0.065536);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(0.065536), 0.065536);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(1.0), 1.048576);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(2.0), 2.097152);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(4.0), 4.194304);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(1e6), 0.065536 * 1024);
  EXPECT_EQ(*q::set_integration_time(1.0), cmd("SetIntegrationTime 1.048576\n"));
}

TEST(QtegraCodec, ParamMapRoundTrips) {
  EXPECT_EQ(q::hardware_name("y_symmetry"), "Y-Symmetry Set");
  EXPECT_EQ(q::canonical_name("Y-Symmetry Set"), "y_symmetry");
  EXPECT_FALSE(q::hardware_name("nope"));
  EXPECT_FALSE(q::canonical_name("nope"));
  EXPECT_EQ(q::param_names().size(), 19u);
  for (const auto& n : q::param_names()) {
    EXPECT_EQ(q::canonical_name(*q::hardware_name(n.canonical)), n.canonical);
  }
}

TEST(QtegraCodec, DecodeOk) {
  EXPECT_TRUE(q::decode_ok(to_bytes("OK\n")));
  EXPECT_TRUE(q::decode_ok(to_bytes("OK\r\n")));
  expect_protocol(q::decode_ok(to_bytes("ERROR: bad detector\n")));
  expect_protocol(q::decode_ok(to_bytes("NOPE\n")));
  expect_protocol(q::decode_ok(to_bytes("OK")));
  expect_protocol(q::decode_ok(to_bytes("")));
}

TEST(QtegraCodec, DecodeNumber) {
  EXPECT_DOUBLE_EQ(*q::decode_number(to_bytes("4.5\n")), 4.5);
  EXPECT_DOUBLE_EQ(*q::decode_number(to_bytes("-1.2E-3\r\n")), -1.2e-3);
  expect_protocol(q::decode_number(to_bytes("abc\n")));
  expect_protocol(q::decode_number(to_bytes("\n")));
  expect_protocol(q::decode_number(to_bytes("1.0")));
  expect_protocol(q::decode_number(to_bytes("ERROR: x\n")));
}

TEST(QtegraCodec, DecodeBool) {
  EXPECT_TRUE(*q::decode_bool(to_bytes("True\n")));
  EXPECT_FALSE(*q::decode_bool(to_bytes("False\n")));
  expect_protocol(q::decode_bool(to_bytes("yes\n")));
}

TEST(QtegraCodec, DecodePairs) {
  auto p = q::decode_pairs(to_bytes("H2,0.0012,H1,-3.4E-3\n"));
  ASSERT_TRUE(p);
  ASSERT_EQ(p->size(), 2u);
  EXPECT_EQ((*p)[0].first, "H2");
  EXPECT_DOUBLE_EQ((*p)[0].second, 0.0012);
  EXPECT_EQ((*p)[1].first, "H1");
  EXPECT_DOUBLE_EQ((*p)[1].second, -3.4e-3);
  EXPECT_TRUE(q::decode_pairs(to_bytes("\n"))->empty());
  expect_protocol(q::decode_pairs(to_bytes("H2,1,H1\n")));
  expect_protocol(q::decode_pairs(to_bytes("H2,x\n")));
  expect_protocol(q::decode_pairs(to_bytes(",1\n")));
  expect_protocol(q::decode_pairs(to_bytes("ERROR: no data\n")));
}

// Synthetic trace: hand-written, not a bench capture.
TEST(QtegraCodec, SyntheticTraceReplays) {
  std::ifstream in(std::string(PYCHRON_TRACES_DIR) + "/thermo/qtegra_read_data.trace");
  ASSERT_TRUE(in.is_open());
  std::string line;
  std::vector<std::pair<std::string, std::string>> frames;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string t, dir, hex;
    ls >> t >> dir >> hex;
    std::string s;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) s.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    frames.emplace_back(dir, s);
  }
  ASSERT_EQ(frames.size(), 4u);
  EXPECT_EQ(frames[0].second, to_string(q::get_data()->tx));
  auto pairs = q::decode_pairs(to_bytes(frames[1].second));
  ASSERT_TRUE(pairs);
  EXPECT_EQ(pairs->size(), 2u);
  EXPECT_EQ(frames[2].second, to_string(q::set_magnet_dac(4.5)->tx));
  EXPECT_TRUE(q::decode_ok(to_bytes(frames[3].second)));
}
