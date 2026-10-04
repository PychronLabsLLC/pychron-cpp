#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "pychron/systems/canvas/loader.hpp"

namespace pychron::canvas {
namespace {

// Every element kind from spec section 6.
constexpr std::string_view kFullCanvas = R"toml(
[canvas]
origin = [0, 0]
size = [1000, 700]
connection_width = 6

[colors]
valve = "#1e90ff"
pipette = "#cccccc"

[[valve]]
name = "A"
pos = [100, 200]

[[valve]]
name = "B"
pos = [200, 200]

[[manual_valve]]
name = "M1"
pos = [300, 200]

[[rough_valve]]
name = "R1"
pos = [400, 200]

[[switch]]
name = "pump_power"
pos = [500, 100]

[[gauge]]
name = "IG1"
pos = [450, 350]

[[stage]]
name = "bone"
pos = [150, 300]
size = [80, 40]
volume = 12.5
fill = true

[[stage]]
name = "spec"
pos = [800, 300]
size = [120, 60]
display_name = "Spectrometer"
use_symbol = true
symbol = "spectrometer"

[[pipette]]
name = "air"
display_name = ""
pos = [600, 400]
vlabel = "Air Pipette"

[[connection]]
start = "A"
end = "B"

[[connection]]
start = "M1"
end = "R1"
orientation = "h"

[[elbow]]
start = "B"
end = "bone"
corner = "ul"

[[tee]]
left = "bone"
right = "spec"
mid = "IG1"

[[cross]]
left = "R1"
right = "air"
top = "M1"
bottom = "spec"

[[label]]
text = "Furnace side"
pos = [100, 50]
font = "Arial 12"

[[image]]
path = "logo.png"
pos = [900, 20]

[legend]
pos = [20, 650]
)toml";

std::uint32_t line_of(std::string_view text, std::string_view needle) {
  const auto pos = text.find(needle);
  return static_cast<std::uint32_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n') + 1);
}

bool has_diag(const CanvasLoadReport& r, std::string_view field, std::string_view message_part) {
  return std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [&](const config::Diagnostic& d) {
    return d.field == field && d.message.find(message_part) != std::string::npos;
  });
}

CanvasLoadReport load(std::string_view text) { return load_canvas_report_from_string(text, "canvas.toml"); }

TEST(CanvasLoader, ParsesEveryElementKind) {
  auto r = load(kFullCanvas);
  ASSERT_TRUE(r.ok()) << config::to_error(r.diagnostics).what;
  const auto& c = *r.canvas;

  EXPECT_EQ(c.source_file, "canvas.toml");
  EXPECT_EQ(c.canvas.origin, (Point{0, 0}));
  EXPECT_EQ(c.canvas.size, (Size{1000, 700}));
  EXPECT_EQ(c.canvas.connection_width, 6);
  EXPECT_EQ(c.colors.at("valve"), "#1e90ff");
  EXPECT_EQ(c.colors.at("pipette"), "#cccccc");

  ASSERT_EQ(c.valves.size(), 5u);
  EXPECT_EQ(c.valves[0].name, "A");
  EXPECT_EQ(c.valves[0].kind, ValveKind::Valve);
  EXPECT_EQ(c.valves[0].pos, (Point{100, 200}));
  EXPECT_EQ(c.valves[2].name, "M1");
  EXPECT_EQ(c.valves[2].kind, ValveKind::Manual);
  EXPECT_EQ(c.valves[3].kind, ValveKind::Rough);
  EXPECT_EQ(c.valves[4].kind, ValveKind::Switch);
  EXPECT_EQ(c.valves[4].name, "pump_power");

  ASSERT_EQ(c.gauges.size(), 1u);
  EXPECT_EQ(c.gauges[0].name, "IG1");

  ASSERT_EQ(c.stages.size(), 2u);
  EXPECT_EQ(c.stages[0].size, (Size{80, 40}));
  EXPECT_EQ(c.stages[0].volume, 12.5);
  EXPECT_TRUE(c.stages[0].fill);
  EXPECT_FALSE(c.stages[0].use_symbol);
  EXPECT_EQ(c.stages[1].display_name, "Spectrometer");
  EXPECT_EQ(c.stages[0].display_name, std::nullopt);  // unset: the name is the label
  ASSERT_FALSE(c.pipettes.empty());
  EXPECT_EQ(c.pipettes[0].display_name, "");  // set and empty: no label
  EXPECT_TRUE(c.stages[1].use_symbol);
  EXPECT_EQ(c.stages[0].symbol, StageSymbol::None);
  EXPECT_EQ(c.stages[1].symbol, StageSymbol::Spectrometer);
  EXPECT_FALSE(c.stages[1].volume.has_value());

  ASSERT_EQ(c.pipettes.size(), 1u);
  EXPECT_EQ(c.pipettes[0].vlabel, "Air Pipette");

  ASSERT_EQ(c.connections.size(), 2u);
  EXPECT_EQ(c.connections[0].start, "A");
  EXPECT_EQ(c.connections[0].end, "B");
  EXPECT_EQ(c.connections[0].orientation, Orientation::Auto);
  EXPECT_EQ(c.connections[1].orientation, Orientation::Horizontal);

  ASSERT_EQ(c.elbows.size(), 1u);
  EXPECT_EQ(c.elbows[0].corner, Corner::UpperLeft);
  ASSERT_EQ(c.tees.size(), 1u);
  EXPECT_EQ(c.tees[0].mid, "IG1");
  ASSERT_EQ(c.crosses.size(), 1u);
  EXPECT_EQ(c.crosses[0].bottom, "spec");

  ASSERT_EQ(c.labels.size(), 1u);
  EXPECT_EQ(c.labels[0].font, "Arial 12");
  ASSERT_EQ(c.images.size(), 1u);
  EXPECT_EQ(c.images[0].path, "logo.png");
  ASSERT_TRUE(c.legend.has_value());
  EXPECT_EQ(c.legend->pos, (Point{20, 650}));
}

TEST(CanvasLoader, RecordsSourceLines) {
  auto r = load(kFullCanvas);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.canvas->valves[1].loc.line, line_of(kFullCanvas, "[[valve]]\nname = \"B\""));
  EXPECT_EQ(r.canvas->valves[1].path, "valve[1]");
}

TEST(CanvasLoader, DefaultsWhenSectionsOmitted) {
  auto r = load(R"toml(
[[stage]]
name = "bone"
pos = [0, 0]
)toml");
  ASSERT_TRUE(r.ok()) << config::to_error(r.diagnostics).what;
  EXPECT_EQ(r.canvas->canvas.connection_width, 5);
  EXPECT_EQ(r.canvas->stages[0].size, (Size{50, 50}));
  EXPECT_FALSE(r.canvas->legend.has_value());
}

TEST(CanvasLoader, SyntaxErrorIsReported) {
  auto r = load("[[valve]\nname = ");
  EXPECT_FALSE(r.ok());
  ASSERT_FALSE(r.diagnostics.empty());
  EXPECT_EQ(r.diagnostics[0].field, "toml");
}

TEST(CanvasLoader, MissingRequiredFields) {
  constexpr std::string_view text = R"toml(
[[valve]]
pos = [1, 2]

[[stage]]
name = "s"
)toml";
  auto r = load(text);
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(has_diag(r, "valve[0].name", "missing required field"));
  EXPECT_TRUE(has_diag(r, "stage[0].pos", "missing required field"));
  const auto it = std::find_if(r.diagnostics.begin(), r.diagnostics.end(),
                               [](const auto& d) { return d.field == "valve[0].name"; });
  ASSERT_NE(it, r.diagnostics.end());
  EXPECT_EQ(it->loc.line, line_of(text, "[[valve]]"));
  EXPECT_EQ(it->loc.file, "canvas.toml");
}

TEST(CanvasLoader, BadPointShapes) {
  auto r = load(R"toml(
[[valve]]
name = "A"
pos = [1]

[[valve]]
name = "B"
pos = "here"

[[valve]]
name = "C"
pos = [1, "x"]
)toml");
  EXPECT_TRUE(has_diag(r, "valve[0].pos", "2 numbers"));
  EXPECT_TRUE(has_diag(r, "valve[1].pos", "expected array"));
  EXPECT_TRUE(has_diag(r, "valve[2].pos", "2 numbers"));
}

TEST(CanvasLoader, NegativeSizeRejected) {
  auto r = load(R"toml(
[[stage]]
name = "s"
pos = [0, 0]
size = [-1, 10]
)toml");
  EXPECT_TRUE(has_diag(r, "stage[0].size", "positive"));
}

TEST(CanvasLoader, UnknownFieldsAndSections) {
  auto r = load(R"toml(
[[valve]]
name = "A"
pos = [0, 0]
colour = "red"

[[widget]]
name = "w"
)toml");
  EXPECT_TRUE(has_diag(r, "valve[0].colour", "unknown field"));
  EXPECT_TRUE(has_diag(r, "widget", "unknown section"));
}

TEST(CanvasLoader, InvalidEnumValues) {
  auto r = load(R"toml(
[[valve]]
name = "A"
pos = [0, 0]
[[valve]]
name = "B"
pos = [0, 0]
[[connection]]
start = "A"
end = "B"
orientation = "diagonal"
[[elbow]]
start = "A"
end = "B"
corner = "middle"
[[stage]]
name = "S"
pos = [0, 0]
symbol = "toaster"
)toml");
  EXPECT_TRUE(has_diag(r, "connection[0].orientation", "invalid value 'diagonal'"));
  EXPECT_TRUE(has_diag(r, "elbow[0].corner", "invalid value 'middle'"));
  EXPECT_TRUE(has_diag(r, "stage[0].symbol", "invalid value 'toaster'"));
}

TEST(CanvasLoader, StageSymbols) {
  const auto r = load_canvas_from_string(R"toml(
[[stage]]
name = "a"
pos = [0, 0]
symbol = "spectrometer"
[[stage]]
name = "b"
pos = [0, 0]
symbol = "quadrupole"
[[stage]]
name = "c"
pos = [0, 0]
symbol = "laser"
)toml",
                                         "canvas.toml");
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_EQ(r->stages.size(), 3u);
  EXPECT_EQ(r->stages[0].symbol, StageSymbol::Spectrometer);
  EXPECT_EQ(r->stages[1].symbol, StageSymbol::Quadrupole);
  EXPECT_EQ(r->stages[2].symbol, StageSymbol::Laser);
}

TEST(CanvasLoader, DuplicateNamesAcrossElementKinds) {
  auto r = load(R"toml(
[[valve]]
name = "A"
pos = [0, 0]
[[stage]]
name = "A"
pos = [5, 5]
)toml");
  EXPECT_TRUE(has_diag(r, "stage[0].name", "duplicate element name 'A'"));
}

TEST(CanvasLoader, ConnectionEndpointsMustResolve) {
  auto r = load(R"toml(
[[valve]]
name = "A"
pos = [0, 0]
[[connection]]
start = "A"
end = "nowhere"
[[tee]]
left = "A"
right = "ghost"
mid = "A"
)toml");
  EXPECT_TRUE(has_diag(r, "connection[0].end", "unknown element 'nowhere'"));
  EXPECT_TRUE(has_diag(r, "tee[0].right", "unknown element 'ghost'"));
}

TEST(CanvasLoader, SelfConnectionRejected) {
  auto r = load(R"toml(
[[valve]]
name = "A"
pos = [0, 0]
[[connection]]
start = "A"
end = "A"
)toml");
  EXPECT_TRUE(has_diag(r, "connection[0].end", "connects 'A' to itself"));
}

TEST(CanvasLoader, SwitchesAndDecorationsCannotBeConnected) {
  auto r = load(R"toml(
[[switch]]
name = "pump_power"
pos = [0, 0]
[[valve]]
name = "A"
pos = [0, 0]
[[connection]]
start = "A"
end = "pump_power"
)toml");
  EXPECT_TRUE(has_diag(r, "connection[0].end", "switch 'pump_power' is not plumbing"));
}

TEST(CanvasLoader, MissingFileReported) {
  auto r = load_canvas_report("/definitely/not/here/canvas.toml");
  EXPECT_FALSE(r.ok());
  ASSERT_EQ(r.diagnostics.size(), 1u);
  EXPECT_EQ(r.diagnostics[0].field, "file");
}

TEST(CanvasLoader, ResultFormCarriesConfigError) {
  auto bad = load_canvas_from_string("[[valve]]\npos = [0, 0]\n", "c.toml");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().kind, ErrorKind::Config);
  EXPECT_NE(bad.error().what.find("c.toml:1:valve[0].name"), std::string::npos) << bad.error().what;

  auto good = load_canvas_from_string(kFullCanvas, "c.toml");
  ASSERT_TRUE(good.has_value());
  EXPECT_EQ(good->valves.size(), 5u);
}

TEST(CanvasLoader, OpenValveColorDefaultsToGreen) {
  auto r = load("[[stage]]\nname = \"bone\"\npos = [0, 0]\n");
  ASSERT_TRUE(r.ok()) << config::to_error(r.diagnostics).what;
  EXPECT_EQ(r.canvas->canvas.open_valve_color, OpenValveColor::Green);
}

TEST(CanvasLoader, OpenValveColorParsesInheritAndGreen) {
  auto inherit = load("[canvas]\nopen_valve_color = \"inherit\"\n");
  ASSERT_TRUE(inherit.ok()) << config::to_error(inherit.diagnostics).what;
  EXPECT_EQ(inherit.canvas->canvas.open_valve_color, OpenValveColor::Inherit);
  auto green = load("[canvas]\nopen_valve_color = \"green\"\n");
  ASSERT_TRUE(green.ok());
  EXPECT_EQ(green.canvas->canvas.open_valve_color, OpenValveColor::Green);
}

TEST(CanvasLoader, BadOpenValveColorIsDiagnosticWithLine) {
  const std::string text = "[canvas]\nsize = [100, 100]\nopen_valve_color = \"blue\"\n";
  auto r = load(text);
  ASSERT_FALSE(r.ok());
  ASSERT_TRUE(has_diag(r, "canvas.open_valve_color", "green | inherit"));
  for (const auto& d : r.diagnostics) {
    if (d.field == "canvas.open_valve_color") {
      EXPECT_EQ(d.loc.line, line_of(text, "open_valve_color"));
    }
  }
  auto wrong_type = load("[canvas]\nopen_valve_color = true\n");
  EXPECT_FALSE(wrong_type.ok());
}

}  // namespace
}  // namespace pychron::canvas
