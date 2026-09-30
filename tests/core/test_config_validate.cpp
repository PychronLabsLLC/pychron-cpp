#include <gtest/gtest.h>

#include <string>

#include "config_fixtures.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/core/config/validate.hpp"

using namespace pychron;
using namespace pychron::config;
using pychron::test::has;
using pychron::test::line_of;

namespace {

std::string valve(std::string_view name, std::string_view address, std::string_view extra = "",
                  std::string_view actuator = "act") {
  return "[[valves]]\nname = \"" + std::string(name) + "\"\nactuator = \"" + std::string(actuator) +
         "\"\naddress = \"" + std::string(address) + "\"\n" + std::string(extra);
}

LoadReport load(const std::string& body) {
  return load_report_from_string(std::string(test::kPreamble) + body, "f.toml");
}

std::string at(const std::string& body, std::string_view needle, std::string_view rest) {
  const auto text = std::string(test::kPreamble) + body;
  return "f.toml:" + std::to_string(line_of(text, needle)) + ":" + std::string(rest);
}

}  // namespace

TEST(ConfigValidate, ValidConfigHasNoDiagnostics) {
  auto rep = load(valve("A", "1", "interlocks = [\"B\"]\n") + valve("B", "2", "interlocks = [\"A\"]\n") +
                  "[[manual_valves]]\nname = \"M1\"\n" + valve("C", "3", "positive_interlocks = [\"M1\"]\n"));
  EXPECT_TRUE(rep.ok()) << testing::PrintToString(test::formatted(rep.diagnostics));
}

TEST(ConfigValidate, UnknownDriverTransport) {
  auto rep = load_report_from_string("[system]\nname = \"x\"\n[drivers.d]\nkind = \"k\"\ntransport = \"nope\"\n", "f.toml");
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:5:drivers.d.transport: unknown transport 'nope'"));
}

TEST(ConfigValidate, UnknownActuator) {
  const auto body = valve("A", "1", "", "ghost");
  auto rep = load(body);
  EXPECT_TRUE(has(rep.diagnostics, at(body, "actuator = \"ghost\"", "valves[0].actuator: unknown actuator 'ghost'")));
}

TEST(ConfigValidate, AddressesUniquePerActuator) {
  const auto body = valve("A", "1") + valve("B", "1") + "[drivers.act2]\nkind = \"proxr_relay\"\ntransport = \"bus\"\n";
  auto rep = load(body + valve("C", "1", "", "act2"));
  ASSERT_EQ(rep.diagnostics.size(), 1u) << testing::PrintToString(test::formatted(rep.diagnostics));
  EXPECT_EQ(rep.diagnostics[0].field, "valves[1].address");
  EXPECT_EQ(rep.diagnostics[0].message, "address '1' on actuator 'act' already used by valve 'A'");
  EXPECT_EQ(rep.diagnostics[0].loc.line, 12u + 4u + 3u);  // B's address line
}

TEST(ConfigValidate, SelfInterlock) {
  const auto body = valve("A", "1", "interlocks = [\"A\"]\n");
  auto rep = load(body);
  EXPECT_TRUE(has(rep.diagnostics, at(body, "interlocks", "valves[0].interlocks[0]: valve 'A' cannot interlock with itself")));
}

TEST(ConfigValidate, UnknownInterlockTargets) {
  const auto body = valve("A", "1", "interlocks = [\"Z\"]\npositive_interlocks = [\"Q\"]\n");
  auto rep = load(body);
  EXPECT_TRUE(has(rep.diagnostics, at(body, "interlocks = ", "valves[0].interlocks[0]: unknown valve 'Z'")));
  EXPECT_TRUE(has(rep.diagnostics, at(body, "positive_interlocks", "valves[0].positive_interlocks[0]: unknown valve 'Q'")));
}

TEST(ConfigValidate, InterlockAndPositiveInterlockConflict) {
  auto rep = load(valve("A", "1", "interlocks = [\"B\"]\npositive_interlocks = [\"B\"]\n") + valve("B", "2"));
  ASSERT_EQ(rep.diagnostics.size(), 1u);
  EXPECT_EQ(rep.diagnostics[0].message, "valve 'B' is both an interlock and a positive interlock");
}

TEST(ConfigValidate, PositiveInterlockCycle) {
  const auto body = valve("A", "1", "positive_interlocks = [\"B\"]\n") +
                    valve("B", "2", "positive_interlocks = [\"C\"]\n") +
                    valve("C", "3", "positive_interlocks = [\"A\"]\n");
  auto rep = load(body);
  ASSERT_EQ(rep.diagnostics.size(), 1u) << testing::PrintToString(test::formatted(rep.diagnostics));
  EXPECT_EQ(rep.diagnostics[0].field, "valves[2].positive_interlocks[0]");
  EXPECT_EQ(rep.diagnostics[0].message, "positive interlock cycle: A -> B -> C -> A");
}

TEST(ConfigValidate, MutualNegativeInterlocksAreAllowed) {
  auto rep = load(valve("A", "1", "interlocks = [\"B\"]\n") + valve("B", "2", "interlocks = [\"A\"]\n"));
  EXPECT_TRUE(rep.ok());
}

TEST(ConfigValidate, DuplicateValveNamesAcrossManualValves) {
  const auto body = valve("A", "1") + "[[manual_valves]]\nname = \"A\"\n";
  auto rep = load(body);
  ASSERT_EQ(rep.diagnostics.size(), 1u);
  EXPECT_EQ(rep.diagnostics[0].field, "manual_valves[0].name");
  EXPECT_EQ(rep.diagnostics[0].message, "duplicate valve name 'A'");
}

TEST(ConfigValidate, GaugeChecks) {
  const auto body = std::string("[[gauges]]\nname = \"G\"\ndriver = \"gc\"\nchannel = 5\n") +
                    "[[gauges]]\nname = \"G\"\ndriver = \"nope\"\n" +
                    "[[gauges]]\nname = \"H\"\ndriver = \"gc\"\nalarm_low = 1e-3\nalarm_high = 1e-4\n";
  auto rep = load(body);
  EXPECT_TRUE(has(rep.diagnostics, at(body, "channel = 5", "gauges[0].channel: channel 5 is not declared by driver 'gc'")));
  EXPECT_TRUE(has(rep.diagnostics, at(body, "driver = \"nope\"", "gauges[1].driver: unknown driver 'nope'")));
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:17:gauges[1].name: duplicate gauge name 'G'"));
  EXPECT_TRUE(has(rep.diagnostics, at(body, "alarm_low", "gauges[2].alarm_low: alarm_low must be less than alarm_high")));
  EXPECT_EQ(rep.diagnostics.size(), 4u);
}

TEST(ConfigValidate, PipetteChecks) {
  auto rep = load(valve("P1", "1") + "[[pipettes]]\nname = \"air\"\ninner = \"P1\"\nouter = \"P1\"\n");
  ASSERT_EQ(rep.diagnostics.size(), 1u);
  EXPECT_EQ(rep.diagnostics[0].message, "inner and outer must be different valves");
}

// validate() works on configs built in code too (no TOML involved).
TEST(ConfigValidate, WorksOnHandBuiltConfig) {
  SystemConfig c;
  c.system.name = "x";
  ValveConfig a;
  a.name = "A";
  a.path = "valves[0]";
  a.loc = SourceLoc{"mem", 3, 1};
  a.actuator = "missing";
  a.address = "1";
  c.valves.push_back(a);
  auto ds = validate(c);
  ASSERT_EQ(ds.size(), 1u);
  EXPECT_EQ(to_string(ds[0]), "mem:3:valves[0].actuator: unknown actuator 'missing'");
}
