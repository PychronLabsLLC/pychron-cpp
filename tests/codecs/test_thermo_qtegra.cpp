#include "pychron/codecs/thermo_qtegra.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>

using namespace pychron;
namespace q = pychron::codec::qtegra;

namespace {

// Replies are framed by CR or LF (pychron strips replies, no read terminator).
const ReadSpec kReply = ReadSpec::until_any("\r\n");

// Default write terminator is "\r" (pychron Communicator.write_terminator).
codec::Command cmd(std::string_view s) { return codec::Command::ascii(std::string(s) + "\r", kReply); }

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
  const std::vector<std::string> dets{"H1", "AX", "CDD"};
  const std::vector<std::string> params{"Y-Symmetry Set", "Trap Voltage Readback"};
  EXPECT_EQ(*q::set_magnet_dac(4.5), cmd("SetMagnetDAC 4.5"));
  EXPECT_EQ(*q::get_magnet_dac(), cmd("GetMagnetDAC"));
  EXPECT_EQ(*q::get_magnet_moving(), cmd("GetMagnetMoving"));
  EXPECT_EQ(*q::blank_beam(true), cmd("BlankBeam True"));
  EXPECT_EQ(*q::blank_beam(false), cmd("BlankBeam False"));
  EXPECT_EQ(*q::protect_detector("H1", true), cmd("ProtectDetector H1,On"));
  EXPECT_EQ(*q::protect_detector("H1", false), cmd("ProtectDetector H1,Off"));
  EXPECT_EQ(*q::protect_detector_parameter("H1", true), cmd("SetParameter ProtectDetector,H1,On"));
  EXPECT_EQ(*q::protect_detector_parameter("H1", false), cmd("SetParameter ProtectDetector,H1,Off"));
  EXPECT_EQ(*q::set_deflection("CDD", 120), cmd("SetDeflection CDD,120"));
  EXPECT_EQ(*q::get_deflection("CDD"), cmd("GetDeflection CDD"));
  EXPECT_EQ(*q::get_deflections(dets), cmd("GetDeflections H1,AX,CDD"));
  EXPECT_EQ(*q::set_gain("H1", 1.5), cmd("SetGain H1,1.5"));
  EXPECT_EQ(*q::get_gain("H1"), cmd("GetGain H1"));
  EXPECT_EQ(*q::set_ion_counter_voltage(1450), cmd("SetIonCounterVoltage 1450"));
  EXPECT_EQ(*q::get_integration_time(), cmd("GetIntegrationTime"));
  EXPECT_EQ(*q::get_data(), cmd("GetData"));
  EXPECT_EQ(*q::set_hv(9900), cmd("SetHV 9900"));
  EXPECT_EQ(*q::get_high_voltage(), cmd("GetHighVoltage"));
  EXPECT_EQ(*q::set_parameter("Y-Symmetry Set", -1.25), cmd("SetParameter Y-Symmetry Set,-1.25"));
  EXPECT_EQ(*q::get_parameter("Y-Symmetry Set"), cmd("GetParameter Y-Symmetry Set"));
  EXPECT_EQ(*q::get_parameters(params), cmd("GetParameters Y-Symmetry Set,Trap Voltage Readback"));
  EXPECT_EQ(*q::set_y_symmetry(-12.5), cmd("SetYSymmetry -12.5"));
  EXPECT_EQ(*q::set_z_symmetry(3), cmd("SetZSymmetry 3"));
  EXPECT_EQ(*q::set_extraction_lens(85.25), cmd("SetExtractionLens 85.25"));
  EXPECT_EQ(*q::get_extraction_symmetry(), cmd("GetExtractionSymmetry"));
  EXPECT_EQ(*q::set_sub_cup_configuration("B"), cmd("SetSubCupConfiguration B"));
  EXPECT_EQ(*q::reset(), cmd("Reset"));
}

TEST(QtegraCodec, WriteTerminatorIsSelectable) {
  EXPECT_EQ(q::terminator_text(q::kDefaultTerminator), "\r");
  EXPECT_EQ(to_string(q::get_data()->tx), "GetData\r");
  EXPECT_EQ(to_string(q::get_data(q::Terminator::LF)->tx), "GetData\n");
  EXPECT_EQ(to_string(q::get_data(q::Terminator::CRLF)->tx), "GetData\r\n");
  EXPECT_EQ(to_string(q::set_magnet_dac(4.5, q::Terminator::LF)->tx), "SetMagnetDAC 4.5\n");
  EXPECT_EQ(*q::get_data()->reply, q::reply_spec());
}

TEST(QtegraCodec, ReplySpecFramesOnCrOrLf) {
  const auto& spec = q::reply_spec();
  EXPECT_EQ(frame_length(spec, to_bytes("OK\r")), 3u);
  EXPECT_EQ(frame_length(spec, to_bytes("OK\n")), 3u);
  EXPECT_EQ(frame_length(spec, to_bytes("OK\r\n")), 3u);
  // The LF of a CRLF reply is absorbed by the next frame.
  EXPECT_EQ(frame_length(spec, to_bytes("\n4.5\r\n")), 5u);
  EXPECT_FALSE(frame_length(spec, to_bytes("4.5")).has_value());
}

TEST(QtegraCodec, FloatsUseShortestRoundTrip) {
  EXPECT_EQ(q::format_number(0.1), "0.1");
  EXPECT_EQ(q::format_number(1.0 / 3.0), "0.3333333333333333");
  EXPECT_EQ(q::format_number(4.123456789012345), "4.123456789012345");
  EXPECT_EQ(q::format_number(-1.25), "-1.25");
  EXPECT_EQ(q::format_number(120), "120");
  EXPECT_EQ(q::format_number(100000000), "100000000");
  EXPECT_EQ(q::format_number(0.0001), "0.0001");
  EXPECT_EQ(q::format_number(1e-5), "1e-05");
  EXPECT_EQ(q::format_number(1e16), "1e+16");
  EXPECT_EQ(*q::set_magnet_dac(4.123456789012345), cmd("SetMagnetDAC 4.123456789012345"));
}

TEST(QtegraCodec, BadArgumentsAreConfigErrors) {
  const std::vector<std::string> none;
  const std::vector<std::string> bad{"H1", "A,X"};
  expect_config(q::set_magnet_dac(NAN));
  expect_config(q::set_hv(INFINITY));
  expect_config(q::set_gain("", 1));
  expect_config(q::set_deflection("H,1", 1));
  expect_config(q::get_gain("H1\n"));
  expect_config(q::set_integration_time(NAN));
  expect_config(q::get_deflections(none));
  expect_config(q::get_parameters(bad));
  expect_config(q::set_sub_cup_configuration(""));
  expect_config(q::set_y_symmetry(NAN));
}

TEST(QtegraCodec, IntegrationTimeSnapsByLinearDistance) {
  EXPECT_DOUBLE_EQ(q::snap_integration_time(0.0), 0.065536);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(-3), 0.065536);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(NAN), 0.065536);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(0.065536), 0.065536);
  // Linear nearest: 0.19 is closer to 0.131072 (0.059) than 0.262144 (0.072);
  // a log-scale snap would pick 0.262144.
  EXPECT_DOUBLE_EQ(q::snap_integration_time(0.19), 0.131072);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(1.0), 1.048576);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(2.0), 2.097152);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(4.0), 4.194304);
  EXPECT_DOUBLE_EQ(q::snap_integration_time(1e6), 67.108864);
  EXPECT_EQ(*q::set_integration_time(1.0), cmd("SetIntegrationTime 1.048576"));
}

TEST(QtegraCodec, ParamMapRoundTrips) {
  EXPECT_EQ(q::hardware_name("y_symmetry"), "Y-Symmetry Set");
  EXPECT_EQ(q::canonical_name("Y-Symmetry Set"), "y_symmetry");
  EXPECT_FALSE(q::hardware_name("nope"));
  EXPECT_FALSE(q::canonical_name("nope"));
  for (const auto& n : q::param_names()) {
    EXPECT_EQ(q::canonical_name(*q::hardware_name(n.canonical)), n.canonical);
    EXPECT_EQ(q::canonical_name(n.hardware), n.canonical);
  }
}

TEST(QtegraCodec, ParamMapHasPychronNames) {
  // Readbacks (ThermoSource read_*).
  EXPECT_EQ(q::readback_name("trap_voltage"), "Trap Voltage Readback");
  EXPECT_EQ(q::readback_name("trap_current"), "Trap Current Readback");
  EXPECT_EQ(q::readback_name("emission"), "Source Current Readback");
  EXPECT_FALSE(q::readback_name("y_symmetry"));
  EXPECT_EQ(q::canonical_name("Trap Voltage Readback"), "trap_voltage");
  EXPECT_EQ(q::canonical_name("Source Current Readback"), "emission");
  EXPECT_EQ(q::hardware_name("trap_voltage"), "Trap Voltage Set");
  // Helix DAC names.
  EXPECT_EQ(q::hardware_name("flatapole"), "DAC_1_0_(Flata-Pole)");
  EXPECT_EQ(q::hardware_name("rotation_quad"), "RotationQuad");
  EXPECT_EQ(q::hardware_name("pole_n"), "DAC_0_0_(Pole-N)");
  EXPECT_EQ(q::hardware_name("pole_s"), "DAC_0_4_(Pole-S)");
  EXPECT_EQ(q::hardware_name("horizontal_symmetry"), "Horizontal Symmetry Set");
  EXPECT_EQ(q::canonical_name("Rotation Quad"), "rotation_quad");
  // Legacy names kept as aliases.
  EXPECT_EQ(q::canonical_name("Flatapole Set"), "flatapole");
  EXPECT_EQ(q::canonical_name("Pole N Set"), "pole_n");
  EXPECT_EQ(q::canonical_name("H-Symmetry Set"), "horizontal_symmetry");
}

TEST(QtegraCodec, DecodeOk) {
  EXPECT_TRUE(q::decode_ok(to_bytes("OK\r")));
  EXPECT_TRUE(q::decode_ok(to_bytes("OK\n")));
  EXPECT_TRUE(q::decode_ok(to_bytes("OK\r\n")));
  EXPECT_TRUE(q::decode_ok(to_bytes("\nok\r")));
  EXPECT_TRUE(q::decode_ok(to_bytes(" Ok ")));
  EXPECT_TRUE(q::decode_ok(to_bytes("OK")));
  expect_protocol(q::decode_ok(to_bytes("ERROR: bad detector\r")));
  expect_protocol(q::decode_ok(to_bytes("NOPE\r")));
  expect_protocol(q::decode_ok(to_bytes("")));
  expect_protocol(q::decode_ok(to_bytes("\r\n")));
}

TEST(QtegraCodec, DecodeNumber) {
  EXPECT_DOUBLE_EQ(*q::decode_number(to_bytes("4.5\r")), 4.5);
  EXPECT_DOUBLE_EQ(*q::decode_number(to_bytes("-1.2E-3\r\n")), -1.2e-3);
  EXPECT_DOUBLE_EQ(*q::decode_number(to_bytes("\n 7 \n")), 7.0);
  EXPECT_DOUBLE_EQ(*q::decode_number(to_bytes("1.0")), 1.0);
  expect_protocol(q::decode_number(to_bytes("abc\r")));
  expect_protocol(q::decode_number(to_bytes("\r")));
  expect_protocol(q::decode_number(to_bytes("ERROR: x\r")));
}

TEST(QtegraCodec, DecodeBoolUsesPychronToBool) {
  for (const char* s : {"True", "true", "TRUE", "t", "yes", "Y", "1", "ok", "OK", "open"}) {
    auto r = q::decode_bool(to_bytes(std::string(s) + "\r\n"));
    ASSERT_TRUE(r) << s;
    EXPECT_TRUE(*r) << s;
  }
  for (const char* s : {"False", "false", "F", "no", "n", "0", "closed"}) {
    auto r = q::decode_bool(to_bytes(std::string(s) + "\r"));
    ASSERT_TRUE(r) << s;
    EXPECT_FALSE(*r) << s;
  }
  expect_protocol(q::decode_bool(to_bytes("maybe\r")));
  expect_protocol(q::decode_bool(to_bytes("\r")));
  expect_protocol(q::decode_bool(to_bytes("ERROR: x\r")));
}

TEST(QtegraCodec, DecodeNamedValues) {
  const std::vector<std::string> dets{"H1", "AX", "CDD"};
  auto p = q::decode_named_values(to_bytes("12.5,-3,1E2\r\n"), dets);
  ASSERT_TRUE(p);
  ASSERT_EQ(p->size(), 3u);
  EXPECT_EQ((*p)[0].first, "H1");
  EXPECT_DOUBLE_EQ((*p)[0].second, 12.5);
  EXPECT_EQ((*p)[1].first, "AX");
  EXPECT_DOUBLE_EQ((*p)[1].second, -3);
  EXPECT_EQ((*p)[2].first, "CDD");
  EXPECT_DOUBLE_EQ((*p)[2].second, 100);
  expect_protocol(q::decode_named_values(to_bytes("12.5,-3\r"), dets));
  expect_protocol(q::decode_named_values(to_bytes("H1,12.5,AX,-3,CDD,1\r"), dets));
  expect_protocol(q::decode_named_values(to_bytes("1,x,3\r"), dets));
  expect_protocol(q::decode_named_values(to_bytes("\r"), dets));
  expect_protocol(q::decode_named_values(to_bytes("ERROR: no\r"), dets));
}

TEST(QtegraCodec, DecodeDataTagged) {
  auto p = q::decode_data(to_bytes("H2,0.0012,H1,-3.4E-3\r\n"));
  ASSERT_TRUE(p);
  ASSERT_EQ(p->size(), 2u);
  EXPECT_EQ((*p)[0].first, "H2");
  EXPECT_DOUBLE_EQ((*p)[0].second, 0.0012);
  EXPECT_EQ((*p)[1].first, "H1");
  EXPECT_DOUBLE_EQ((*p)[1].second, -3.4e-3);
  EXPECT_TRUE(q::decode_data(to_bytes("\r"))->empty());
  expect_protocol(q::decode_data(to_bytes("H2,1,H1\r")));
  expect_protocol(q::decode_data(to_bytes("H2,x\r")));
  expect_protocol(q::decode_data(to_bytes(",1\r")));
  expect_protocol(q::decode_data(to_bytes("ERROR: no data\r")));
  // pychron read_intensities: any reply containing "ERROR" is an error.
  expect_protocol(q::decode_data(to_bytes("H2,1,ERROR,2\r")));
}

TEST(QtegraCodec, DecodeDataUntagged) {
  auto p = q::decode_data(to_bytes("1,100,3,0.01,0.02,0.03\r"), q::kDefaultDetectorOrder);
  ASSERT_TRUE(p);
  ASSERT_EQ(p->size(), 6u);
  EXPECT_EQ((*p)[0].first, "H2");
  EXPECT_DOUBLE_EQ((*p)[0].second, 1);
  EXPECT_EQ((*p)[2].first, "AX");
  EXPECT_DOUBLE_EQ((*p)[2].second, 3);
  EXPECT_EQ((*p)[5].first, "CDD");
  EXPECT_DOUBLE_EQ((*p)[5].second, 0.03);
  const std::array<std::string_view, 2> order{"H1", "AX"};
  auto two = q::decode_data(to_bytes("5,6\n"), order);
  ASSERT_TRUE(two);
  EXPECT_EQ((*two)[1].first, "AX");
  EXPECT_TRUE(q::decode_data(to_bytes("\r"), order)->empty());
  expect_protocol(q::decode_data(to_bytes("5\r"), order));
  expect_protocol(q::decode_data(to_bytes("5,x\r"), order));
  expect_protocol(q::decode_data(to_bytes("5,ERROR\r"), order));
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
  auto pairs = q::decode_data(to_bytes(frames[1].second));
  ASSERT_TRUE(pairs);
  EXPECT_EQ(pairs->size(), 2u);
  EXPECT_EQ(frames[2].second, to_string(q::set_magnet_dac(4.5)->tx));
  EXPECT_TRUE(q::decode_ok(to_bytes(frames[3].second)));
}

TEST(QtegraCodec, DecodeRequestSplitsVerbAndArguments) {
  auto bare = q::decode_request(to_bytes("GetMagnetDAC\r"));
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->verb, "GetMagnetDAC");
  EXPECT_TRUE(bare->args.empty());
  auto one = q::decode_request(to_bytes("SetMagnetDAC 5.001\r\n"));
  ASSERT_TRUE(one);
  EXPECT_EQ(one->verb, "SetMagnetDAC");
  EXPECT_EQ(one->args, (std::vector<std::string>{"5.001"}));
  auto named = q::decode_request(to_bytes("SetParameter Trap Voltage Set, 5\n"));
  ASSERT_TRUE(named);
  EXPECT_EQ(named->verb, "SetParameter");
  EXPECT_EQ(named->args, (std::vector<std::string>{"Trap Voltage Set", "5"}));
  expect_protocol(q::decode_request(to_bytes("\r")));
}

TEST(QtegraCodec, ServerRepliesRoundTripThroughDecoders) {
  EXPECT_EQ(q::encode_ok(), to_bytes("OK\r\n"));
  EXPECT_EQ(q::encode_number(4.5), to_bytes("4.5\r\n"));
  EXPECT_EQ(q::encode_bool(true), to_bytes("True\r\n"));
  EXPECT_EQ(q::encode_bool(false), to_bytes("False\r\n"));
  EXPECT_EQ(q::encode_error("bad"), to_bytes("ERROR: bad\r\n"));
  EXPECT_TRUE(q::decode_ok(q::encode_ok()));
  EXPECT_DOUBLE_EQ(*q::decode_number(q::encode_number(1.048576)), 1.048576);
  EXPECT_TRUE(*q::decode_bool(q::encode_bool(true)));
  expect_protocol(q::decode_number(q::encode_error("bad")));
}

TEST(QtegraCodec, DecodeAckAcceptsAnythingButError) {
  for (std::string_view reply : {"OK\r\n", "ok\r", "5.001\r\n", "True\n", "\r\n", "", "  \r"}) {
    EXPECT_TRUE(q::decode_ack(to_bytes(reply))) << reply;
  }
  expect_protocol(q::decode_ack(to_bytes("ERROR: bad\r\n")));
  expect_protocol(q::decode_ack(to_bytes("  ERROR\r")));
  expect_protocol(q::decode_ack(q::encode_error("detector not found")));
}

TEST(QtegraCodec, ValidateNameMatchesEncoderRule) {
  EXPECT_TRUE(q::validate_name("H1"));
  EXPECT_TRUE(q::validate_name("Trap Voltage Set"));
  expect_config(q::validate_name(""));
  expect_config(q::validate_name("H1,AX"));
  expect_config(q::validate_name("H1\r"));
}
