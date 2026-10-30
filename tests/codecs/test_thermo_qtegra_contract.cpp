// What the codec sends, checked against RemoteControlServer's own definitions.
// tests/traces/thermo/protocol.json is a copy of protocol.json in the qtegra
// repository: replace it when that file changes, and change the codec to match.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/codecs/thermo_qtegra.hpp"

using namespace pychron;
namespace q = pychron::codec::qtegra;
using nlohmann::json;

namespace {

// Commands by name.
const std::map<std::string, json>& contract() {
  static const std::map<std::string, json> commands = [] {
    std::map<std::string, json> out;
    std::ifstream in(std::string(PYCHRON_TRACES_DIR) + "/thermo/protocol.json");
    const json parsed = json::parse(in, nullptr, false);
    if (parsed.is_discarded()) return out;
    for (const auto& command : parsed["commands"]) out[command["name"].get<std::string>()] = command;
    return out;
  }();
  return commands;
}

bool is_type(const json& arg, const std::string& value) {
  const auto type = arg["type"].get<std::string>();
  if (value.empty()) return false;
  if (type == "number") return codec::parse_decimal(value).has_value();
  if (type == "integer") return value.find_first_not_of("0123456789") == std::string::npos;
  if (type == "word") return value.find(' ') == std::string::npos;
  if (type == "enum") {
    for (const auto& allowed : arg["values"]) {
      if (allowed == value) return true;
    }
    return false;
  }
  return true;  // text
}

// Empty when `rest`, what follows the command name and its space, fits the
// arguments of `command`; otherwise what is wrong.
std::string shape_problem(const json& command, std::string rest) {
  const auto& args = command["args"];
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (rest.empty()) {
      if (arg["optional"].get<bool>()) return {};
      return "missing " + arg["name"].get<std::string>();
    }
    if (i > 0) {
      const auto separator = arg["separator"].get<std::string>();
      if (!rest.starts_with(separator)) return "no separator before " + arg["name"].get<std::string>();
      rest.erase(0, separator.size());
    }
    if (arg["repeated"].get<bool>()) {
      std::stringstream fields(rest);
      std::string field;
      while (std::getline(fields, field, ',')) {
        if (!is_type(arg, field)) return "bad " + arg["name"].get<std::string>() + ": \"" + field + "\"";
      }
      rest.clear();
      continue;
    }
    const auto next = i + 1 < args.size() ? rest.find(args[i + 1]["separator"].get<std::string>()) : std::string::npos;
    const std::string value = rest.substr(0, next);
    if (!is_type(arg, value)) return "bad " + arg["name"].get<std::string>() + ": \"" + value + "\"";
    rest.erase(0, value.size());
  }
  if (!rest.empty()) return "left over: \"" + rest + "\"";
  return {};
}

// Empty when the wire text fits the contract.
std::string problem(const std::string& tx) {
  const std::string line = tx.substr(0, tx.find_first_of("\r\n"));
  const auto space = line.find(' ');
  const std::string verb = line.substr(0, space);
  const auto it = contract().find(verb);
  if (it == contract().end()) return "no command " + verb;
  return shape_problem(it->second, space == std::string::npos ? "" : line.substr(space + 1));
}

// One of each encoder, with both forms where a flag changes the text.
std::vector<Result<codec::Command>> every_encoder() {
  const std::vector<std::string> detectors{"H1", "AX"};
  const std::vector<std::string> parameters{"Y-Symmetry Set", "Trap Voltage Readback"};
  return {q::keep_alive(),
          q::describe(),
          q::ping(),
          q::set_magnet_dac(4.5),
          q::get_magnet_dac(),
          q::get_magnet_moving(),
          q::blank_beam(true),
          q::blank_beam(false),
          q::protect_detector("H1", true),
          q::protect_detector("CDD", false),
          q::set_deflection("CDD", 120),
          q::get_deflection("CDD"),
          q::get_deflections(detectors),
          q::set_gain("H1", 1.5),
          q::get_gain("H1"),
          q::set_ion_counter_voltage(1450),
          q::set_integration_time(1.048576),
          q::get_integration_time(),
          q::get_data(),
          q::set_hv(9900),
          q::get_high_voltage(),
          q::set_parameter("Trap Voltage Set", 5.5),
          q::get_parameter("Trap Voltage Set"),
          q::get_parameters(parameters),
          q::set_y_symmetry(-12.5),
          q::set_z_symmetry(3),
          q::set_extraction_lens(85.25),
          q::get_extraction_symmetry(),
          q::activate_cup_configuration("Ar", "Ar_CDD"),
          q::open_valve("Valve 1_9 Set"),
          q::close_valve("Valve 1_9 Set"),
          q::get_valve_state("Valve 1_9 Set")};
}

}  // namespace

TEST(QtegraContract, TheCopyOfTheContractLoads) {
  EXPECT_EQ(contract().size(), 49U);
  std::ifstream in(std::string(PYCHRON_TRACES_DIR) + "/thermo/protocol.json");
  const json parsed = json::parse(in, nullptr, false);
  ASSERT_FALSE(parsed.is_discarded());
  EXPECT_EQ(parsed["protocol"].get<int>(), q::kProtocolVersion);
}

TEST(QtegraContract, EveryEncoderSendsACommandOfTheContract) {
  for (const auto& command : every_encoder()) {
    ASSERT_TRUE(command) << to_string(command.error());
    const std::string tx = to_string(command->tx);
    EXPECT_EQ(problem(tx), "") << tx;
  }
}

// A new encoder missing from every_encoder() would go unchecked, so the source
// is read for the verbs it names.
TEST(QtegraContract, EveryVerbInTheCodecSourceIsCovered) {
  std::ifstream in(PYCHRON_QTEGRA_CODEC_SOURCE);
  ASSERT_TRUE(in);
  std::stringstream source;
  source << in.rdbuf();
  const std::string text = source.str();
  std::set<std::string> covered;
  for (const auto& command : every_encoder()) {
    ASSERT_TRUE(command);
    const std::string tx = to_string(command->tx);
    covered.insert(tx.substr(0, tx.find_first_of(" \r\n")));
  }
  const std::regex call(R"re((?:simple|with_\w+)\([^"\n]*"([A-Z]\w+))re");
  std::size_t verbs = 0;
  for (auto it = std::sregex_iterator(text.begin(), text.end(), call); it != std::sregex_iterator(); ++it) {
    const std::string verb = (*it)[1].str();
    ++verbs;
    EXPECT_TRUE(contract().contains(verb)) << "no command " << verb;
    EXPECT_TRUE(covered.contains(verb)) << verb << " is not in every_encoder()";
  }
  EXPECT_GE(verbs, 25U);  // the pattern still finds the encoders
}

TEST(QtegraContract, WhatRemoteControlServerRefusesIsCaught) {
  for (const auto& [tx, why] : std::vector<std::pair<std::string, std::string>>{
           {"SetYSymmetry 1\r", "no command SetYSymmetry"},
           {"SetExtractionLens 1\r", "no command SetExtractionLens"},
           {"GetExtractionSymmetry\r", "no command GetExtractionSymmetry"},
           {"SetSubCupConfiguration B\r", "no command SetSubCupConfiguration"},
           {"Reset\r", "no command Reset"},
           {"BlankBeam True\r", "bad blank: \"True\""},
           {"SetParameter ProtectDetector,H1,On\r", "bad value: \"H1,On\""},
           {"ProtectDetector H1 On\r", "bad detector: \"H1 On\""},
           {"ActivateCupConfiguration Ar\r", "missing sub_cup"},
           {"SetMagnetDAC volts\r", "bad dac: \"volts\""},
           {"GetData now\r", "left over: \"now\""},
       }) {
    EXPECT_EQ(problem(tx), why) << tx;
  }
}
