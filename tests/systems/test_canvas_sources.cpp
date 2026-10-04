// What colours a region of the canvas: the connected element with the
// highest precedence, as in legacy pychron (pump 120, pipette and laser 100,
// tank 90, spectrometer 80, getter 70; a plain volume is not a source).

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "pychron/systems/canvas/canvas.hpp"
#include "pychron/systems/canvas/loader.hpp"

using namespace pychron;
using namespace pychron::canvas;

namespace {

const char* kCanvas = R"toml(
[[stage]]
name = "bone"
pos = [0, 0]

[[stage]]
name = "jan"
pos = [0, 0]
symbol = "spectrometer"

[[stage]]
name = "quad"
pos = [0, 0]
symbol = "quadrupole"

[[stage]]
name = "ion_pump"
pos = [0, 0]
symbol = "ion_pump"

[[stage]]
name = "turbo"
pos = [0, 0]
symbol = "turbo"

[[stage]]
name = "gp50"
pos = [0, 0]
symbol = "getter"

[[stage]]
name = "co2"
pos = [0, 0]
symbol = "laser"

[[stage]]
name = "furnace"
pos = [0, 0]
kind = "laser"          # a laser drawn as a plain box

[[stage]]
name = "air_tank"
pos = [0, 0]
kind = "tank"
color = "#00C3FF"

[[stage]]
name = "felix"
pos = [0, 0]
symbol = "spectrometer"
precedence = 200        # this lab wants the spectrometer to win

[[stage]]
name = "dead_getter"
pos = [0, 0]
symbol = "getter"
precedence = 0          # drawn as a getter, colours nothing

[[pipette]]
name = "air"
pos = [0, 0]

[[pipette]]
name = "cocktail"
pos = [0, 0]
precedence = 5
color = "#102030"
)toml";

Canvas fixture() {
  auto r = load_canvas_report_from_string(kCanvas, "canvas.toml");
  EXPECT_TRUE(r.ok()) << (r.diagnostics.empty() ? "" : r.diagnostics.front().message);
  return *r.canvas;
}

std::string winner(const Canvas& c, std::set<std::string> volumes) {
  const auto all = sources(c);
  const Source* s = dominant(all, volumes);
  return s == nullptr ? "" : s->name;
}

}  // namespace

TEST(CanvasSources, KindAndPrecedenceDefaultFromWhatTheStageIs) {
  const auto all = sources(fixture());
  auto of = [&](const char* name) { return all.at(name); };
  EXPECT_EQ(of("turbo").kind, SourceKind::Pump);
  EXPECT_EQ(of("turbo").precedence, 120);
  EXPECT_EQ(of("ion_pump").kind, SourceKind::Pump);
  EXPECT_EQ(of("ion_pump").precedence, 120);
  EXPECT_EQ(of("co2").kind, SourceKind::Laser);
  EXPECT_EQ(of("co2").precedence, 100);
  EXPECT_EQ(of("furnace").kind, SourceKind::Laser);
  EXPECT_EQ(of("furnace").precedence, 100);
  EXPECT_EQ(of("air").kind, SourceKind::Pipette);
  EXPECT_EQ(of("air").precedence, 100);
  EXPECT_EQ(of("air_tank").kind, SourceKind::Tank);
  EXPECT_EQ(of("air_tank").precedence, 90);
  EXPECT_EQ(of("jan").kind, SourceKind::Spectrometer);
  EXPECT_EQ(of("jan").precedence, 80);
  EXPECT_EQ(of("quad").kind, SourceKind::Spectrometer);
  EXPECT_EQ(of("gp50").kind, SourceKind::Getter);
  EXPECT_EQ(of("gp50").precedence, 70);
}

TEST(CanvasSources, APlainVolumeOrPrecedenceZeroIsNotASource) {
  const auto all = sources(fixture());
  EXPECT_FALSE(all.contains("bone"));
  EXPECT_FALSE(all.contains("dead_getter"));
}

TEST(CanvasSources, TheFileOverridesPrecedenceAndColour) {
  const auto all = sources(fixture());
  EXPECT_EQ(all.at("felix").precedence, 200);
  EXPECT_EQ(all.at("cocktail").precedence, 5);
  EXPECT_EQ(all.at("cocktail").color, std::optional<std::string>("#102030"));
  EXPECT_EQ(all.at("air_tank").color, std::optional<std::string>("#00C3FF"));
  EXPECT_EQ(all.at("jan").color, std::nullopt);
}

TEST(CanvasSources, TheHighestPrecedenceInARegionWins) {
  const auto c = fixture();
  EXPECT_EQ(winner(c, {"bone"}), "");                          // nothing connected: no source
  EXPECT_EQ(winner(c, {"bone", "gp50"}), "gp50");
  EXPECT_EQ(winner(c, {"bone", "gp50", "jan"}), "jan");        // 80 over 70
  EXPECT_EQ(winner(c, {"bone", "jan", "ion_pump"}), "ion_pump");  // 120 over 80
  EXPECT_EQ(winner(c, {"bone", "air_tank", "jan"}), "air_tank");  // 90 over 80
  EXPECT_EQ(winner(c, {"felix", "ion_pump", "turbo"}), "felix");  // 200, from the file
  EXPECT_EQ(winner(c, {"jan"}), "jan");                        // alone, it is its own
  EXPECT_EQ(winner(c, {"no-such-volume", "gp50"}), "gp50");
}

TEST(CanvasSources, TiesGoByKindThenName) {
  const auto c = fixture();
  EXPECT_EQ(winner(c, {"co2", "air"}), "air");            // both 100: pipette before laser
  EXPECT_EQ(winner(c, {"furnace", "co2"}), "co2");        // both lasers: by name
  EXPECT_EQ(winner(c, {"turbo", "ion_pump"}), "ion_pump");
  EXPECT_EQ(winner(c, {"jan", "quad"}), "jan");
}

TEST(CanvasSources, BadValuesAreReported) {
  auto r = load_canvas_report_from_string(R"toml(
[[stage]]
name = "a"
pos = [0, 0]
kind = "toaster"
[[stage]]
name = "b"
pos = [0, 0]
precedence = -1
[[stage]]
name = "c"
pos = [0, 0]
color = "blue"
[[pipette]]
name = "d"
pos = [0, 0]
color = "#12345"
)toml",
                                   "canvas.toml");
  EXPECT_FALSE(r.ok());
  auto said = [&](const std::string& field, const std::string& part) {
    for (const auto& d : r.diagnostics)
      if (d.field == field && d.message.find(part) != std::string::npos) return true;
    return false;
  };
  EXPECT_TRUE(said("stage[0].kind", "invalid value 'toaster'"));
  EXPECT_TRUE(said("stage[1].precedence", "0 or more"));
  EXPECT_TRUE(said("stage[2].color", "#rrggbb"));
  EXPECT_TRUE(said("pipette[0].color", "#rrggbb"));
}
