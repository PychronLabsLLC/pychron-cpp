// build_line_sim: the simulated lab of a line without the line (sim serve
// spec 4.2). That ExtractionLine still builds the same lab through it is
// what the line's own sim tests check.
#include "pychron/systems/line_sim.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/loader.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;
namespace fs = std::filesystem;

const fs::path kExamples = PYCHRON_EXAMPLE_CONFIGS_DIR;

constexpr const char* kSystem = R"(
[system]
name = "t"
)";

// left --A-- right, and a valve D drawn with one side only.
constexpr const char* kCanvas = R"(
[[valve]]
name = "A"
pos = [0, 0]

[[valve]]
name = "D"
pos = [1, 0]

[[stage]]
name = "left"
pos = [0, 0]

[[stage]]
name = "right"
pos = [0, 0]

[[connection]]
start = "left"
end = "A"

[[connection]]
start = "A"
end = "right"

[[connection]]
start = "right"
end = "D"
)";

config::SystemConfig system_config() {
  auto cfg = config::load_system_config_from_string(kSystem, "t.toml");
  EXPECT_TRUE(cfg) << cfg.error().what;
  return *cfg;
}

canvas::Canvas canvas_model(const std::string& toml = kCanvas) {
  auto c = canvas::load_canvas_from_string(toml, "c.toml");
  EXPECT_TRUE(c) << c.error().what;
  return *c;
}

}  // namespace

TEST(LineSim, BuildsExampleLine) {
  auto cfg = config::load_system_config(kExamples / "extraction_line.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  auto canvas = canvas::load_canvas(kExamples / "canvas.toml");
  ASSERT_TRUE(canvas) << canvas.error().what;
  ManualClock clock;
  LineSimOptions options;
  options.sim_file = sim_file_beside(kExamples / "extraction_line.toml", true).read;
  auto built = build_line_sim(*cfg, &*canvas, clock, std::move(options));
  ASSERT_TRUE(built) << built.error().what;
  ASSERT_NE(built->system, nullptr);
  EXPECT_TRUE(built->system->spectrometer_volume().has_value());
  for (const auto& stage : canvas->stages) EXPECT_TRUE(built->system->has_volume(stage.name)) << stage.name;
}

TEST(LineSim, WithoutCanvasIsEmpty) {
  ManualClock clock;
  auto built = build_line_sim(system_config(), nullptr, clock);
  ASSERT_TRUE(built) << built.error().what;
  EXPECT_FALSE(built->system->spectrometer_volume().has_value());
  EXPECT_FALSE(built->system->has_volume("left"));
}

TEST(LineSim, TildeInValveNameRefused) {
  std::string text(kCanvas);
  for (std::size_t at = text.find("\"A\""); at != std::string::npos; at = text.find("\"A\"", at + 1))
    text.replace(at, 3, "\"A~B\"");
  ManualClock clock;
  const auto canvas = canvas_model(text);
  auto built = build_line_sim(system_config(), &canvas, clock);
  ASSERT_FALSE(built);
  EXPECT_EQ(built.error().kind, ErrorKind::Config);
  EXPECT_NE(built.error().what.find("A~B"), std::string::npos) << built.error().what;
}

TEST(LineSim, NotesSayValvesWithoutPhysics) {
  ManualClock clock;
  const auto canvas = canvas_model();
  auto built = build_line_sim(system_config(), &canvas, clock);
  ASSERT_TRUE(built) << built.error().what;
  bool said = false;
  for (const auto& note : built->notes) {
    if (note.message.find("carry no gas") == std::string::npos) continue;
    said = true;
    EXPECT_EQ(note.level, LogLevel::Info);
    EXPECT_NE(note.message.find("D ("), std::string::npos) << note.message;
  }
  EXPECT_TRUE(said);
}

TEST(LineSim, RefusedNetworkFails) {
  ManualClock clock;
  const auto canvas = canvas_model();
  LineSimOptions options;
  options.sim.initial_pressures = {{"left", -1e-6}};
  auto built = build_line_sim(system_config(), &canvas, clock, std::move(options));
  ASSERT_FALSE(built);
  EXPECT_NE(built.error().what.find("left"), std::string::npos) << built.error().what;
}

TEST(LineSim, SimFileBesideOnlyWithCanvas) {
  const auto dir = fs::temp_directory_path() / "pychron_line_sim_beside";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const auto line = dir / "extraction_line.toml";
  EXPECT_FALSE(sim_file_beside(line, true).read);
  std::ofstream(dir / "sim.toml") << "";
  const auto with = sim_file_beside(line, true);
  EXPECT_EQ(with.read, dir / "sim.toml");
  EXPECT_TRUE(with.skipped.empty());
  const auto without = sim_file_beside(line, false);
  EXPECT_FALSE(without.read);
  EXPECT_EQ(without.skipped, dir / "sim.toml");
  fs::remove_all(dir);
}
