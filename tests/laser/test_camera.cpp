// cameras.toml: the camera of each extraction device (laser autocenter
// design, section 3).

#include "pychron/laser/camera.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::laser;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

CameraLibrary parse(const std::string& text) { return CameraLibrary::parse(text, "cameras.toml"); }

}  // namespace

TEST(CameraConfig, DefaultsWithAnEmptyTable) {
  const auto lib = parse("[co2]\n");
  ASSERT_TRUE(lib.problems().empty()) << lib.problems().front();
  const CameraConfig* c = lib.find("co2");
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->device, "co2");
  EXPECT_EQ(c->source, CameraSource::Sim);
  EXPECT_DOUBLE_EQ(c->px_per_mm, 23.0);
  EXPECT_FALSE(c->flip_x);
  EXPECT_TRUE(c->flip_y);
  EXPECT_DOUBLE_EQ(c->aim_offset_px.x, 0);
  EXPECT_EQ(c->settle, Duration(200ms));
  EXPECT_DOUBLE_EQ(c->sim_tray_error_mm.x, 0);
  EXPECT_DOUBLE_EQ(c->sim_noise, 0.01);
  EXPECT_EQ(c->sim_width, 200);
  EXPECT_EQ(c->sim_height, 200);
  EXPECT_DOUBLE_EQ(c->tolerance_mm, 0.03);
  EXPECT_DOUBLE_EQ(c->max_step_mm, 0.5);
  EXPECT_EQ(c->max_iterations, 4);
  EXPECT_EQ(c->frames_per_step, 3);
  EXPECT_EQ(c->on_failure, OnAutocenterFailure::Continue);
  EXPECT_DOUBLE_EQ(c->sim_grain_offset_mm.x, 0);
  EXPECT_DOUBLE_EQ(c->sim_glow_drift_mm_per_s.x, 0);
  EXPECT_DOUBLE_EQ(c->sim_glow_sigma_mm, 0.3);
  EXPECT_EQ(lib.devices(), (std::vector<std::string>{"co2"}));
  EXPECT_EQ(lib.find("diode"), nullptr);
}

TEST(CameraConfig, ReadsEveryKey) {
  const auto lib = parse(R"toml(
[co2]
source = "sim"
px_per_mm = 40
flip_x = true
flip_y = false
aim_offset_px = [3, -2.5]
settle_ms = 350

[co2.sim]
tray_error_mm = [0.15, -0.10]
noise = 0.02
width = 320
height = 240

grain_offset_mm = [0.2, 0.1]
glow_drift_mm_per_s = [0.01, -0.02]
glow_sigma_mm = 0.25

[co2.autocenter]
tolerance_mm = 0.05
max_iterations = 6
max_step_mm = 0.25
frames_per_step = 5
on_failure = "fail"

[diode]
source = "recorded"
frames = "recordings/diode-holes"
)toml");
  ASSERT_TRUE(lib.problems().empty()) << lib.problems().front();
  const CameraConfig* c = lib.find("co2");
  ASSERT_NE(c, nullptr);
  EXPECT_DOUBLE_EQ(c->px_per_mm, 40);
  EXPECT_TRUE(c->flip_x);
  EXPECT_FALSE(c->flip_y);
  EXPECT_DOUBLE_EQ(c->aim_offset_px.x, 3);
  EXPECT_DOUBLE_EQ(c->aim_offset_px.y, -2.5);
  EXPECT_EQ(c->settle, Duration(350ms));
  EXPECT_DOUBLE_EQ(c->sim_tray_error_mm.x, 0.15);
  EXPECT_DOUBLE_EQ(c->sim_tray_error_mm.y, -0.10);
  EXPECT_DOUBLE_EQ(c->sim_noise, 0.02);
  EXPECT_EQ(c->sim_width, 320);
  EXPECT_EQ(c->sim_height, 240);
  EXPECT_DOUBLE_EQ(c->sim_grain_offset_mm.x, 0.2);
  EXPECT_DOUBLE_EQ(c->sim_grain_offset_mm.y, 0.1);
  EXPECT_DOUBLE_EQ(c->sim_glow_drift_mm_per_s.x, 0.01);
  EXPECT_DOUBLE_EQ(c->sim_glow_drift_mm_per_s.y, -0.02);
  EXPECT_DOUBLE_EQ(c->sim_glow_sigma_mm, 0.25);
  EXPECT_DOUBLE_EQ(c->tolerance_mm, 0.05);
  EXPECT_EQ(c->max_iterations, 6);
  EXPECT_DOUBLE_EQ(c->max_step_mm, 0.25);
  EXPECT_EQ(c->frames_per_step, 5);
  EXPECT_EQ(c->on_failure, OnAutocenterFailure::Fail);
  const CameraConfig* d = lib.find("diode");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->source, CameraSource::Recorded);
  EXPECT_EQ(d->frames, "recordings/diode-holes");
  EXPECT_EQ(lib.devices(), (std::vector<std::string>{"co2", "diode"}));
}

// A hole seen to the right of the aim point: with the usual camera the stage
// must move +x to bring it under the beam... which way is the map's to say,
// and it follows the flips exactly as the vision library's does.
TEST(CameraConfig, MapFollowsTheFlips) {
  for (bool fx : {false, true}) {
    for (bool fy : {false, true}) {
      CameraConfig c;
      c.px_per_mm = 20;
      c.flip_x = fx;
      c.flip_y = fy;
      const auto got = c.map().to_mm({10, 10});
      const auto want = vision::CameraStageMap::from_scale(20, fx, fy).to_mm({10, 10});
      EXPECT_DOUBLE_EQ(got.x, want.x);
      EXPECT_DOUBLE_EQ(got.y, want.y);
      EXPECT_DOUBLE_EQ(std::abs(got.x), 0.5);
      EXPECT_EQ(got.x < 0, fx);
      EXPECT_EQ(got.y < 0, fy);
    }
  }
}

struct BadCamera {
  const char* text;
  const char* key;
  friend void PrintTo(const BadCamera& b, std::ostream* os) { *os << b.key; }
};

class CameraConfigBad : public ::testing::TestWithParam<BadCamera> {};

TEST_P(CameraConfigBad, RefusesBadValues) {
  const auto lib = parse(GetParam().text);
  ASSERT_EQ(lib.problems().size(), 1u) << GetParam().text;
  const std::string& problem = lib.problems().front();
  EXPECT_EQ(problem.rfind("cameras.toml: ", 0), 0u) << problem;
  EXPECT_NE(problem.find(GetParam().key), std::string::npos) << problem;
  EXPECT_EQ(lib.find("co2"), nullptr);  // a camera that did not load is not half used
  EXPECT_EQ(lib.problems_of("co2"), lib.problems());
  EXPECT_TRUE(lib.problems_of("diode").empty());
}

INSTANTIATE_TEST_SUITE_P(
    Values, CameraConfigBad,
    ::testing::Values(BadCamera{"[co2]\npx_per_mm = 0\n", "co2.px_per_mm"},
                      BadCamera{"[co2]\npx_per_mm = -5\n", "co2.px_per_mm"},
                      BadCamera{"[co2]\npx_per_mm = nan\n", "co2.px_per_mm"},
                      BadCamera{"[co2]\npx_per_mm = \"23\"\n", "co2.px_per_mm"},
                      BadCamera{"[co2]\nflip_x = 1\n", "co2.flip_x"},
                      BadCamera{"[co2]\naim_offset_px = [1]\n", "co2.aim_offset_px"},
                      BadCamera{"[co2]\naim_offset_px = [1, inf]\n", "co2.aim_offset_px"},
                      BadCamera{"[co2]\nsettle_ms = -1\n", "co2.settle_ms"},
                      BadCamera{"[co2]\nsettle_ms = 10001\n", "co2.settle_ms"},
                      BadCamera{"[co2]\nsource = \"usb\"\n", "co2.source"},
                      BadCamera{"[co2]\nsource = \"recorded\"\n", "co2.frames"},
                      BadCamera{"[co2]\nwobble = 1\n", "co2.wobble"},
                      BadCamera{"[co2]\n[co2.sim]\nnoise = 2\n", "co2.sim.noise"},
                      BadCamera{"[co2]\n[co2.sim]\nwidth = 8\n", "co2.sim.width"},
                      BadCamera{"[co2]\n[co2.sim]\nheight = 5000\n", "co2.sim.height"},
                      BadCamera{"[co2]\n[co2.sim]\ntray_error_mm = 0.1\n", "co2.sim.tray_error_mm"},
                      BadCamera{"[co2]\n[co2.sim]\nzoom = 1\n", "co2.sim.zoom"},
                      BadCamera{"[co2]\n[co2.sim]\nglow_sigma_mm = 0\n", "co2.sim.glow_sigma_mm"},
                      BadCamera{"[co2]\n[co2.sim]\ngrain_offset_mm = [1]\n", "co2.sim.grain_offset_mm"},
                      BadCamera{"[co2]\n[co2.sim]\nglow_drift_mm_per_s = \"slow\"\n", "co2.sim.glow_drift_mm_per_s"},
                      BadCamera{"[co2]\n[co2.autocenter]\ntolerance_mm = 0\n", "co2.autocenter.tolerance_mm"},
                      BadCamera{"[co2]\n[co2.autocenter]\nmax_step_mm = 0\n", "co2.autocenter.max_step_mm"},
                      BadCamera{"[co2]\n[co2.autocenter]\nmax_iterations = 0\n", "co2.autocenter.max_iterations"},
                      BadCamera{"[co2]\n[co2.autocenter]\nmax_iterations = 51\n", "co2.autocenter.max_iterations"},
                      BadCamera{"[co2]\n[co2.autocenter]\nframes_per_step = 0\n", "co2.autocenter.frames_per_step"},
                      BadCamera{"[co2]\n[co2.autocenter]\nframes_per_step = 16\n", "co2.autocenter.frames_per_step"},
                      BadCamera{"[co2]\n[co2.autocenter]\non_failure = \"retry\"\n", "co2.autocenter.on_failure"},
                      BadCamera{"[co2]\n[co2.autocenter]\nspeed = 1\n", "co2.autocenter.speed"},
                      BadCamera{"co2 = 3\n", "co2"}));

TEST(CameraLibrary, OneBadTableDoesNotHideTheOthers) {
  const auto lib = parse("[co2]\npx_per_mm = 0\n\n[diode]\npx_per_mm = 30\n");
  EXPECT_EQ(lib.find("co2"), nullptr);
  ASSERT_NE(lib.find("diode"), nullptr);
  EXPECT_DOUBLE_EQ(lib.find("diode")->px_per_mm, 30);
  EXPECT_EQ(lib.problems().size(), 1u);
  EXPECT_EQ(lib.problems_of("co2").size(), 1u);
  EXPECT_TRUE(lib.problems_of("diode").empty());
}

TEST(CameraLibrary, MissingFileIsEmpty) {
  const auto lib = CameraLibrary::load(fs::temp_directory_path() / "pychron_no_such_cameras.toml");
  EXPECT_TRUE(lib.devices().empty());
  EXPECT_TRUE(lib.problems().empty());
}

// A file that is not TOML is a problem for every device: nobody's camera is known.
TEST(CameraLibrary, NotTomlIsEveryDevicesProblem) {
  const auto lib = parse("[co2\npx_per_mm = ");
  EXPECT_TRUE(lib.devices().empty());
  ASSERT_EQ(lib.problems().size(), 1u);
  EXPECT_EQ(lib.problems().front().rfind("cameras.toml: ", 0), 0u);
  EXPECT_EQ(lib.problems_of("co2").size(), 1u);
  EXPECT_EQ(lib.problems_of("anything").size(), 1u);
}

TEST(CameraLibrary, LoadsAFile) {
  std::random_device rd;
  const auto file = fs::temp_directory_path() / ("pychron_cameras_" + std::to_string(rd()) + ".toml");
  std::ofstream(file) << "[co2]\npx_per_mm = 31\n";
  const auto lib = CameraLibrary::load(file);
  ASSERT_NE(lib.find("co2"), nullptr);
  EXPECT_DOUBLE_EQ(lib.find("co2")->px_per_mm, 31);
  fs::remove(file);
}
