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

// Live cameras (live camera design, sections 1 and 3).

TEST(CameraConfig, ReadsALiveOpenCvCamera) {
  const auto lib = parse(R"([co2]
source = "opencv"
use = "view"
px_per_mm = 40

[co2.opencv]
device = 1
width = 1280
height = 720
fps = 30
channel = "g"
rotate = 90
roi = [10, 20, 300, 200]
timeout_ms = 400
)");
  ASSERT_TRUE(lib.problems().empty()) << lib.problems().front();
  const CameraConfig* c = lib.find("co2");
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->source, CameraSource::OpenCv);
  EXPECT_TRUE(c->live());
  EXPECT_EQ(c->use, CameraUse::View);
  const vision::CameraRequest r = c->request();
  EXPECT_EQ(r.backend, "opencv");
  EXPECT_EQ(r.device, "1");
  EXPECT_EQ(r.width, 1280);
  EXPECT_EQ(r.height, 720);
  EXPECT_DOUBLE_EQ(r.fps, 30);
  EXPECT_EQ(r.shape.channel, vision::SourceConfig::Channel::G);
  EXPECT_EQ(r.shape.rotate, 90);
  EXPECT_EQ(r.shape.roi.x, 10);
  EXPECT_EQ(r.shape.roi.h, 200);
  EXPECT_EQ(c->live_timeout, Duration(400ms));
  // by default: camera 0, as it comes, and it centers
  const auto plain = parse("[co2]\nsource = \"opencv\"\n");
  ASSERT_TRUE(plain.problems().empty()) << plain.problems().front();
  EXPECT_EQ(plain.find("co2")->request().device, "0");
  EXPECT_EQ(plain.find("co2")->use, CameraUse::Center);
  EXPECT_EQ(plain.find("co2")->live_timeout, Duration(1000ms));
  // a file is a device too
  const auto file = parse("[co2]\nsource = \"opencv\"\n[co2.opencv]\ndevice = \"clips/a.mp4\"\n");
  ASSERT_TRUE(file.problems().empty()) << file.problems().front();
  EXPECT_EQ(file.find("co2")->request().device, "clips/a.mp4");
}

TEST(CameraConfig, ReadsAPylonCameraAheadOfItsDriver) {
  const auto lib = parse(R"([co2]
source = "pylon"

[co2.pylon]
serial = "40012345"
exposure_us = 8000
gain_db = 3.5
pixel_format = "Mono12"
packet_size = 8192
timeout_ms = 2000
)");
  ASSERT_TRUE(lib.problems().empty()) << lib.problems().front();
  const CameraConfig* c = lib.find("co2");
  EXPECT_EQ(c->source, CameraSource::Pylon);
  EXPECT_TRUE(c->live());
  const vision::CameraRequest r = c->request();
  EXPECT_EQ(r.backend, "pylon");
  EXPECT_EQ(r.device, "40012345");
  EXPECT_EQ(r.options.at("exposure_us"), "8000");
  EXPECT_EQ(r.options.at("gain_db"), "3.5");
  EXPECT_EQ(r.options.at("pixel_format"), "Mono12");
  EXPECT_EQ(r.options.at("packet_size"), "8192");
  EXPECT_EQ(c->live_timeout, Duration(2000ms));
  EXPECT_FALSE(parse("[co2]\n").find("co2")->live());
}

TEST(CameraConfig, RefusesBadLiveValues) {
  for (const char* text : {
           "[co2]\nsource = \"webcam\"\n",
           "[co2]\nuse = \"steer\"\n",
           "[co2]\n[co2.opencv]\ndevice = -1\n",
           "[co2]\n[co2.opencv]\ndevice = true\n",
           "[co2]\n[co2.opencv]\nrotate = 45\n",
           "[co2]\n[co2.opencv]\nchannel = \"x\"\n",
           "[co2]\n[co2.opencv]\nroi = [1, 2, 3]\n",
           "[co2]\n[co2.opencv]\nroi = [1, 2, -3, 4]\n",
           "[co2]\n[co2.opencv]\nwidth = -4\n",
           "[co2]\n[co2.opencv]\ntimeout_ms = 0\n",
           "[co2]\n[co2.opencv]\ntimeout_ms = 99\n",
           "[co2]\n[co2.opencv]\nexposure = 4\n",
           "[co2]\n[co2.pylon]\nexposure_us = 0\n",
           "[co2]\n[co2.pylon]\npacket_size = 10\n",
           "[co2]\n[co2.pylon]\nserial = 7\n",
           "[co2]\n[co2.pylon]\npixel_format = \"\"\n",
       }) {
    const auto lib = parse(text);
    EXPECT_EQ(lib.problems().size(), 1u) << text;
    EXPECT_EQ(lib.find("co2"), nullptr) << text;
  }
}

TEST(CameraUse, ALiveCameraClosesTheLoopOnlyOverARealStage) {
  CameraConfig live;
  live.device = "co2";
  live.source = CameraSource::OpenCv;
  EXPECT_TRUE(usable_for_autocenter(live, false));
  const auto on_sim = usable_for_autocenter(live, true);
  ASSERT_FALSE(on_sim);
  EXPECT_EQ(on_sim.error().kind, ErrorKind::Config);
  EXPECT_NE(on_sim.error().what.find("simulated"), std::string::npos) << on_sim.error().what;
  EXPECT_NE(on_sim.error().what.find("use = \"view\""), std::string::npos) << "it says what to do: " << on_sim.error().what;
  live.source = CameraSource::Pylon;
  EXPECT_TRUE(usable_for_autocenter(live, false));
  EXPECT_FALSE(usable_for_autocenter(live, true));
}

// A video file is not a camera over the stage, whatever plays it.
TEST(CameraUse, AVideoFileNeverClosesTheLoop) {
  CameraConfig clip;
  clip.device = "co2";
  clip.source = CameraSource::OpenCv;
  clip.live_device = "clips/a.mp4";
  const auto r = usable_for_autocenter(clip, false);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("video file"), std::string::npos) << r.error().what;
  clip.live_device = "2";
  EXPECT_TRUE(usable_for_autocenter(clip, false));
}

// What a centering waits for a frame is what an emergency stop may wait too.
TEST(CameraConfig, ACameraThatCentersWaitsTwoSecondsAtMost) {
  const char* slow = "[co2]\nsource = \"opencv\"\n[co2.opencv]\ntimeout_ms = 5000\n";
  auto lib = parse(slow);
  ASSERT_EQ(lib.problems().size(), 1u);
  EXPECT_NE(lib.problems().front().find("timeout_ms"), std::string::npos) << lib.problems().front();
  lib = parse(std::string(slow) + "");
  // for looking only, it may be patient
  lib = parse("[co2]\nsource = \"opencv\"\nuse = \"view\"\n[co2.opencv]\ntimeout_ms = 5000\n");
  EXPECT_TRUE(lib.problems().empty());
  // and too little time is no time to take a frame in
  lib = parse("[co2]\nsource = \"opencv\"\nuse = \"view\"\n[co2.opencv]\ntimeout_ms = 20\n");
  EXPECT_EQ(lib.problems().size(), 1u);
}

TEST(CameraConfig, ItsGeometryNamesWhatAScaleWasMeasuredWith) {
  const auto a = parse("[co2]\nsource = \"opencv\"\nuse = \"view\"\n[co2.opencv]\nrotate = 90\n");
  const auto b = parse("[co2]\nsource = \"opencv\"\nuse = \"view\"\n[co2.opencv]\nrotate = 180\n");
  const auto c = parse("[co2]\nsource = \"opencv\"\nuse = \"view\"\npx_per_mm = 99\n[co2.opencv]\nrotate = 90\n");
  EXPECT_NE(a.find("co2")->geometry(), b.find("co2")->geometry());
  EXPECT_EQ(a.find("co2")->geometry(), c.find("co2")->geometry()) << "what the scale replaces is not part of it";
  EXPECT_NE(a.find("co2")->geometry(), parse("[co2]\n").find("co2")->geometry());
}

TEST(CameraUse, ACameraForLookingNeverClosesTheLoop) {
  for (const CameraSource source : {CameraSource::Sim, CameraSource::OpenCv, CameraSource::Pylon}) {
    CameraConfig view;
    view.device = "co2";
    view.source = source;
    view.use = CameraUse::View;
    for (const bool simulated : {true, false}) {
      const auto r = usable_for_autocenter(view, simulated);
      ASSERT_FALSE(r);
      EXPECT_NE(r.error().what.find("looking"), std::string::npos) << r.error().what;
    }
  }
}
