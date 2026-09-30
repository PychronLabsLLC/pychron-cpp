#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/cross_validate.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/network_graph.hpp"

namespace pychron::canvas {
namespace {

constexpr std::string_view kSystem = R"toml(
[system]
name = "t"
[transports.bus]
kind = "sim"
[drivers.act]
kind = "proxr_relay"
transport = "bus"
[drivers.ig]
kind = "pfeiffer_maxigauge"
transport = "bus"
[[valves]]
name = "A"
actuator = "act"
address = "1"
[[valves]]
name = "B"
actuator = "act"
address = "2"
[[valves]]
name = "R1"
actuator = "act"
address = "3"
[[manual_valves]]
name = "M1"
[[switches]]
name = "pump_power"
actuator = "act"
address = "9"
[[gauges]]
name = "IG1"
driver = "ig"
[[pipettes]]
name = "air"
inner = "A"
outer = "B"
)toml";

config::SystemConfig system() {
  auto s = config::load_system_config_from_string(kSystem, "extraction_line.toml");
  EXPECT_TRUE(s.has_value()) << s.error().what;
  return *s;
}

Canvas canvas(std::string_view text) {
  auto c = load_canvas_from_string(text, "canvas.toml");
  EXPECT_TRUE(c.has_value()) << c.error().what;
  return *c;
}

bool has(const std::vector<config::Diagnostic>& ds, std::string_view field, std::string_view part) {
  return std::any_of(ds.begin(), ds.end(), [&](const config::Diagnostic& d) {
    return d.field == field && d.message.find(part) != std::string::npos;
  });
}

constexpr std::string_view kComplete = R"toml(
[[valve]]
name = "A"
pos = [0, 0]
[[valve]]
name = "B"
pos = [0, 0]
[[rough_valve]]
name = "R1"
pos = [0, 0]
[[manual_valve]]
name = "M1"
pos = [0, 0]
[[switch]]
name = "pump_power"
pos = [0, 0]
[[gauge]]
name = "IG1"
pos = [0, 0]
[[pipette]]
name = "air"
pos = [0, 0]
)toml";

TEST(CanvasCrossValidate, CompleteCanvasIsClean) {
  const auto r = cross_validate(canvas(kComplete), system());
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(r.errors.empty());
  EXPECT_TRUE(r.warnings.empty());
}

TEST(CanvasCrossValidate, CanvasValveMissingFromSystemIsError) {
  constexpr std::string_view text = R"toml(
[[valve]]
name = "A"
pos = [0, 0]
[[valve]]
name = "Z"
pos = [0, 0]
)toml";
  const auto r = cross_validate(canvas(text), system());
  EXPECT_FALSE(r.ok());
  ASSERT_TRUE(has(r.errors, "valve[1].name", "valve 'Z' is not defined in extraction_line.toml"));
  EXPECT_EQ(r.errors[0].loc.file, "canvas.toml");
  EXPECT_EQ(r.errors[0].loc.line, 6u);  // the `name = "Z"` line
}

TEST(CanvasCrossValidate, ValveKindMismatchIsError) {
  // M1 is a manual valve in the system but drawn as an actuated valve; A is
  // actuated but drawn as manual.
  const auto r = cross_validate(canvas(R"toml(
[[valve]]
name = "M1"
pos = [0, 0]
[[manual_valve]]
name = "A"
pos = [0, 0]
)toml"),
                                system());
  EXPECT_TRUE(has(r.errors, "valve[0].name", "manual valve"));
  EXPECT_TRUE(has(r.errors, "manual_valve[0].name", "manual valve 'A' is not defined"));
}

TEST(CanvasCrossValidate, UnknownGaugeAndPipetteAreErrors) {
  const auto r = cross_validate(canvas(R"toml(
[[gauge]]
name = "IG9"
pos = [0, 0]
[[pipette]]
name = "cocktail"
pos = [0, 0]
)toml"),
                                system());
  EXPECT_TRUE(has(r.errors, "gauge[0].name", "gauge 'IG9' is not defined"));
  EXPECT_TRUE(has(r.errors, "pipette[0].name", "pipette 'cocktail' is not defined"));
}

TEST(CanvasCrossValidate, SystemValvesMissingFromCanvasWarn) {
  const auto r = cross_validate(canvas(R"toml(
[[valve]]
name = "A"
pos = [0, 0]
)toml"),
                                system());
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(has(r.warnings, "valves[1]", "valve 'B' is not drawn on the canvas"));
  EXPECT_TRUE(has(r.warnings, "valves[2]", "valve 'R1' is not drawn on the canvas"));
  EXPECT_TRUE(has(r.warnings, "manual_valves[0]", "manual valve 'M1' is not drawn on the canvas"));
  ASSERT_EQ(r.warnings.size(), 3u);
  EXPECT_EQ(r.warnings[0].loc.file, "extraction_line.toml");
}

TEST(CanvasCrossValidate, ResultFormFailsOnErrorsOnly) {
  auto ok = check_canvas(canvas("[[valve]]\nname = \"A\"\npos = [0, 0]\n"), system());
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->size(), 3u);  // warnings returned on success

  auto bad = check_canvas(canvas("[[valve]]\nname = \"Q\"\npos = [0, 0]\n"), system());
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().kind, ErrorKind::Config);
  EXPECT_NE(bad.error().what.find("canvas.toml:2:valve[0].name"), std::string::npos) << bad.error().what;
}

TEST(ExampleConfigs, LoadCrossValidateAndBuildGraph) {
  const std::filesystem::path dir = PYCHRON_EXAMPLE_CONFIGS_DIR;
  auto sys = config::load_system_config(dir / "extraction_line.toml");
  ASSERT_TRUE(sys.has_value()) << sys.error().what;
  auto c = load_canvas(dir / "canvas.toml");
  ASSERT_TRUE(c.has_value()) << c.error().what;

  const auto report = cross_validate(*c, *sys);
  EXPECT_TRUE(report.errors.empty()) << config::to_error(report.errors).what;
  EXPECT_TRUE(report.warnings.empty()) << config::to_error(report.warnings).what;

  const auto g = systems::NetworkGraph::from_canvas(*c);
  systems::ValveStates open;
  for (const auto& v : sys->valves) {
    EXPECT_TRUE(g.is_valve(v.name)) << v.name;
    open[v.name] = ValveState::Open;
  }
  EXPECT_FALSE(g.connected_to("bone", {}).contains("spec"));
  EXPECT_TRUE(g.connected_to("bone", {{"A", ValveState::Open}, {"B", ValveState::Open}}).contains("spec"));
  // Every actuated valve open joins every volume except those behind the
  // manual valve.
  EXPECT_FALSE(g.connected_to("bone", open).contains("rough"));
  open["M1"] = ValveState::Open;
  EXPECT_EQ(g.connected_volumes(open).size(), 1u);
}

TEST(CanvasCrossValidate, CanvasSwitchMissingFromSystemIsError) {
  constexpr std::string_view text = R"toml(
[[switch]]
name = "heater_relay"
pos = [0, 0]
)toml";
  const auto r = cross_validate(canvas(text), system());
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(has(r.errors, "switch[0].name", "switch 'heater_relay' is not defined"));
}

TEST(CanvasCrossValidate, SystemSwitchNotDrawnIsWarning) {
  const auto r = cross_validate(canvas("[[valve]]\nname = \"A\"\npos = [0, 0]\n"), system());
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(has(r.warnings, "switches[0]", "switch 'pump_power' is not drawn on the canvas"));
}

}  // namespace
}  // namespace pychron::canvas
