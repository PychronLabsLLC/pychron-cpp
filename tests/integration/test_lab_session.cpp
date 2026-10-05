// LabSession on the sim lab: a scratch copy of configs/examples run on a
// simulated clock pumped 400x, as `elctl exp run --sim --sim-speed 400` does.

#include <gtest/gtest.h>

#include <random>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/clock_pump.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/vision/fixture.hpp"
#include "pychron/vision/synth.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"

namespace pychron::experiment::lab {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

template <class F>
bool eventually(F done, std::chrono::seconds limit = 30s) {
  const auto until = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) return false;
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

class LabSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("pychron-session-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                                     std::to_string(std::random_device{}()));
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_, fs::copy_options::recursive);
    fs::remove_all(dir_ / "data");
    prepare_lab();

    systems::ExtractionLine::Options options;
    options.clock = &clock_;
    options.force_sim = true;
    options.scheduler.threads = 0;
    options.run_scheduler = false;
    options.state_file = dir_ / "line.state.toml";
    auto line = systems::ExtractionLine::load(dir_ / "extraction_line.toml", dir_ / "canvas.toml", options);
    ASSERT_TRUE(line) << line.error().what;
    line_ = std::move(*line);
    pump_.drive(&line_->scheduler());
    ASSERT_TRUE(line_->start());

    auto spec = spectrometer::load_spectrometer_for_app(
        dir_ / "spectrometer.sim-integrated.toml",
        spectrometer::SpectrometerContext{clock_, line_->scheduler(), line_->bus()},
        spectrometer::SpectrometerBringup{.sim_beam_from_table = true});
    ASSERT_TRUE(spec) << spec.error().what;
    spec_ = std::move(*spec);
    scan_ = std::make_unique<spectrometer::ScanService>(*spec_, line_->bus(), clock_);

    lab_ = load_lab({dir_, dir_ / "extraction_line.toml", dir_ / "spectrometer.sim-integrated.toml"});
    ASSERT_TRUE(lab_.problems.empty()) << lab_.problems.front();
    auto q = load_queue_file((dir_ / "experiment.toml").string(), lab_.ids);
    ASSERT_TRUE(q) << q.error().what;
    queue_ = *q;
    // Without embedded Python the example scripts cannot run.
    if (!scripting::make_script_host()->available()) {
      for (auto& r : queue_.runs) {
        r.extraction.script.clear();
        r.post_measurement.reset();
      }
    }
    session_ = std::make_unique<LabSession>(lab_, hardware(), SessionOptions{dir_ / "data", {}, {}});
    subs_.push_back(line_->bus().subscribe<QueueEnded>([this](const QueueEnded& e) {
      std::lock_guard lock(mutex_);
      ended_.push_back(e);
    }));
    subs_.push_back(line_->bus().subscribe<executor::RunStarted>([this](const executor::RunStarted&) { ++started_; }));
  }

  // Changes to the scratch lab before anything is loaded from it.
  virtual void prepare_lab() {}
  virtual SessionHardware hardware() { return SessionHardware{*line_, spec_.get(), scan_.get(), {}}; }

  QueueSpec laser_queue() {
    auto q = load_queue_file((dir_ / "experiment.laser.toml").string(), lab_.ids);
    EXPECT_TRUE(q) << q.error().what;
    return q ? *q : QueueSpec{};
  }
  extraction::ChromiumSim& laser_sim(std::string_view driver) {
    auto* sim = line_->sim() != nullptr ? line_->sim()->chromium(driver) : nullptr;
    if (sim == nullptr) throw std::logic_error("no simulated Chromium for " + std::string(driver));
    return *sim;
  }
  static int count(const std::vector<std::string>& log, std::string_view command) {
    return static_cast<int>(std::count(log.begin(), log.end(), command));
  }

  void TearDown() override {
    subs_.clear();
    session_.reset();
    scan_.reset();
    pump_.drive(nullptr);
    pump_.stop();
    if (line_) line_->stop();
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
    line_.reset();
    fs::remove_all(dir_);
  }

  std::vector<QueueEnded> ended() {
    std::lock_guard lock(mutex_);
    return ended_;
  }

  fs::path dir_;
  ManualClock clock_{TimePoint{} + std::chrono::hours(1)};
  ClockPump pump_{clock_, 400};
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<spectrometer::Spectrometer> spec_;
  std::unique_ptr<spectrometer::ScanService> scan_;
  Lab lab_;
  QueueSpec queue_;
  std::unique_ptr<LabSession> session_;
  std::mutex mutex_;
  std::vector<QueueEnded> ended_;
  std::atomic<int> started_{0};
  std::vector<SignalBus::Subscription> subs_;
};

TEST_F(LabSessionTest, RunsTheExampleQueueAndPausesTheScan) {
  ASSERT_TRUE(scan_->start(1s));
  ASSERT_TRUE(session_->has_spectrometer());
  ASSERT_TRUE(session_->start(queue_));
  EXPECT_TRUE(session_->running());
  ASSERT_TRUE(eventually([&] { return scan_->paused(); }));
  auto again = session_->start(queue_);
  ASSERT_FALSE(again);
  EXPECT_NE(again.error().what.find("already running"), std::string::npos);

  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  ASSERT_EQ(result->runs.size(), 3u);
  for (const auto& r : result->runs) EXPECT_EQ(r.state, run::RunState::Success) << r.identifier << " " << r.error.value_or("");
  EXPECT_FALSE(session_->running());
  EXPECT_EQ(session_->state(), executor::ExecutorState::Idle);
  EXPECT_EQ(session_->pending_saves(), 0u);
  EXPECT_TRUE(fs::exists(dir_ / "data" / "records" / "bu" / "bu-1.json"));
  EXPECT_TRUE(fs::exists(dir_ / "data" / "executor_state.json"));
  auto row = LabSession::resume_row(dir_ / "data");
  ASSERT_TRUE(row);
  EXPECT_EQ(*row, 3u);

  // QueueEnded comes after the scan is resumed.
  ASSERT_TRUE(eventually([&] { return !ended().empty(); }));
  EXPECT_EQ(ended().front().result.end, executor::QueueEnd::Completed);
  EXPECT_TRUE(scan_->running());
  EXPECT_FALSE(scan_->paused());
}

// --- laser queues (laser system design, sections 5 and 7) --------------------

TEST_F(LabSessionTest, ALaserQueueMovesFiresAndLeavesTheLaserOff) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  auto& sim = laser_sim("co2");
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  ASSERT_EQ(result->runs.size(), 2u);
  for (const auto& r : result->runs) EXPECT_EQ(r.state, run::RunState::Success) << r.identifier << " " << r.error.value_or("");

  // example-9 is calibrated with its centre at stage (25, 25): hole 3 at
  // (30, 30) mm, hole 7 at (20, 20). The example's camera (cameras.toml) sees
  // the tray 0.15, -0.10 mm from there, and each hole move ends by centring
  // the hole: the beam is fired on the real holes.
  const auto log = sim.log();
  EXPECT_EQ(count(log, "Stage.MoveTo 30000,30000,0,5000,5000,100"), 1) << ::testing::PrintToString(log);
  EXPECT_EQ(count(log, "Stage.MoveTo 20000,20000,0,5000,5000,100"), 1);
  EXPECT_EQ(count(log, "Laser.Output 20"), 2);
  EXPECT_EQ(count(log, "Laser.Fire"), 2);

  struct Move {
    double x, y;
    std::string speeds;
  };
  const auto parse = [](const std::string& line) {
    // "Stage.MoveTo x,y,z,speed x,speed y,speed z"
    Move m{};
    std::istringstream in(line.substr(13));
    long long x = 0, y = 0, z = 0;
    char comma = 0;
    in >> x >> comma >> y >> comma >> z >> comma;
    std::getline(in, m.speeds);
    m.x = static_cast<double>(x) / 1000.0;
    m.y = static_cast<double>(y) / 1000.0;
    return m;
  };
  // Where the stage was when each beam came on, and what it did under it.
  std::vector<Move> last_before_fire;
  std::vector<Move> under_second_beam;
  Move last{};
  std::size_t fires = 0;
  bool firing = false;
  for (const auto& line : log) {
    if (line.starts_with("Stage.MoveTo ")) {
      last = parse(line);
      if (firing && fires == 2) under_second_beam.push_back(last);
    } else if (line == "Laser.Fire") {
      ++fires;
      firing = true;
      last_before_fire.push_back(last);
    } else if (line == "Laser.Stop") {
      firing = false;
    }
  }
  ASSERT_EQ(last_before_fire.size(), 2u);
  EXPECT_NEAR(last_before_fire[0].x, 30.15, 0.03);  // hole 3, where it really is
  EXPECT_NEAR(last_before_fire[0].y, 29.90, 0.03);
  EXPECT_NEAR(last_before_fire[1].x, 20.15, 0.03);  // hole 7
  EXPECT_NEAR(last_before_fire[1].y, 19.90, 0.03);

  // The second run names the hexagon pattern (radius 1 mm, 1 mm/s): with the
  // beam on, the stage goes round it about the centred hole and comes back.
  const double cx = last_before_fire[1].x, cy = last_before_fire[1].y;
  const double h = std::sqrt(3.0) / 2;
  const std::vector<std::pair<double, double>> hexagon{{1, 0}, {0.5, h}, {-0.5, h}, {-1, 0},
                                                       {-0.5, -h}, {0.5, -h}, {1, 0}, {0, 0}};
  ASSERT_EQ(under_second_beam.size(), hexagon.size()) << ::testing::PrintToString(log);
  for (std::size_t i = 0; i < hexagon.size(); ++i) {
    EXPECT_NEAR(under_second_beam[i].x, cx + hexagon[i].first, 0.0011) << i;
    EXPECT_NEAR(under_second_beam[i].y, cy + hexagon[i].second, 0.0011) << i;
  }
  // each side at 1 mm/s along it: the level ones whole, the slanted ones shared
  EXPECT_EQ(under_second_beam[0].speeds, "1000,1000,100");
  EXPECT_EQ(under_second_beam[1].speeds, "500,866,100");
  EXPECT_EQ(under_second_beam[2].speeds, "1000,1000,100");
  EXPECT_NEAR(static_cast<double>(sim.position().x) / 1000.0, cx, 0.0011);
  EXPECT_NEAR(static_cast<double>(sim.position().y) / 1000.0, cy, 0.0011);

  // and what was found is remembered for the two holes
  const laser::TrayMap& map = *lab_.trays.find("example-9");
  const auto remembered = lab_.corrections->load(map, "co2", lab_.calibrations->status(map, "co2").fingerprint);
  ASSERT_TRUE(remembered) << remembered.error().what;
  ASSERT_TRUE(remembered->contains("3"));
  ASSERT_TRUE(remembered->contains("7"));
  EXPECT_NEAR(remembered->at("3").x, 30.15, 0.03);
  EXPECT_NEAR(remembered->at("7").y, 19.90, 0.03);
  EXPECT_TRUE(session_->problems().empty());
  EXPECT_FALSE(sim.firing());
  EXPECT_FALSE(sim.enabled());
  EXPECT_DOUBLE_EQ(sim.output(), 0);
}

// Whether each hole was centred is said as it happens, is in the run's
// summary and is kept in its record.
TEST_F(LabSessionTest, ALaserRunSaysWhetherItsHoleWasCentred) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  std::mutex mutex;
  std::map<std::string, std::vector<std::string>> said;  // by run id
  std::map<std::string, std::size_t> rows, note_rows;
  subs_.push_back(line_->bus().subscribe<run::RunNote>([&](const run::RunNote& e) {
    std::lock_guard lock(mutex);
    said[e.run_id].push_back(e.text);
    note_rows[e.run_id] = e.row;
  }));
  subs_.push_back(line_->bus().subscribe<executor::RunStarted>([&](const executor::RunStarted& e) {
    std::lock_guard lock(mutex);
    rows[e.run_id] = e.row;
  }));
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->runs.size(), 2u);
  const char* holes[] = {"hole 3: centred, moved ", "hole 7: centred, moved "};
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& r = result->runs[i];
    ASSERT_EQ(r.state, run::RunState::Success) << r.error.value_or("");
    const auto centred = [&](const std::vector<std::string>& lines) {
      return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.starts_with(holes[i]); });
    };
    EXPECT_TRUE(centred(r.messages)) << ::testing::PrintToString(r.messages);
    {
      std::lock_guard lock(mutex);
      EXPECT_EQ(rows[r.run_id], i);
      EXPECT_EQ(note_rows[r.run_id], i);
      EXPECT_EQ(said[r.run_id], r.messages);
    }
    const fs::path file = dir_ / "data" / "records" / r.identifier / (r.identifier + "-" + std::to_string(r.aliquot) + ".json");
    std::ifstream in(file);
    ASSERT_TRUE(in) << file;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto rec = record::from_json(text);
    ASSERT_TRUE(rec) << rec.error().what;
    std::vector<std::string> notes;
    for (const auto& e : rec->events)
      if (e.kind == "note") notes.push_back(e.detail);
    EXPECT_TRUE(centred(notes)) << ::testing::PrintToString(notes);
  }
}

// A run whose pattern is a dragonfly: with the beam on, the stage leaves the
// centred hole for the glowing grain (the example's camera has it 0.2, 0.1 mm
// off its hole), stays with it, and comes back; the laser is off after.
TEST_F(LabSessionTest, ADragonflyRunFollowsTheGrainAndLeavesTheLaserOff) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  auto& sim = laser_sim("co2");
  auto q = laser_queue();
  q.runs.at(1).extraction.pattern = "follow";
  ASSERT_TRUE(session_->start(q));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->runs.size(), 2u);
  for (const auto& r : result->runs) EXPECT_EQ(r.state, run::RunState::Success) << r.identifier << " " << r.error.value_or("");

  // The stage moves made while the second beam was on, in mm.
  std::vector<std::pair<double, double>> under_beam;
  std::size_t fires = 0;
  bool firing = false;
  for (const auto& line : sim.log()) {
    if (line == "Laser.Fire") firing = ++fires == 2;
    else if (line == "Laser.Stop") firing = false;
    else if (firing && line.starts_with("Stage.MoveTo ")) {
      std::istringstream in(line.substr(13));
      long long x = 0, y = 0;
      char comma = 0;
      in >> x >> comma >> y;
      under_beam.emplace_back(static_cast<double>(x) / 1000.0, static_cast<double>(y) / 1000.0);
    }
  }
  ASSERT_GE(under_beam.size(), 2u) << ::testing::PrintToString(sim.log());
  // hole 7 really at (20.15, 19.90); the grain 0.2, 0.1 from its centre
  bool reached = false;
  for (const auto& [x, y] : under_beam) reached = reached || std::hypot(x - 20.35, y - 20.00) < 0.08;
  EXPECT_TRUE(reached) << ::testing::PrintToString(under_beam);
  for (const auto& [x, y] : under_beam) EXPECT_LT(std::hypot(x - 20.15, y - 19.90), 2.5 + 0.03);  // inside its perimeter
  // back on the hole at the end, and nothing left on
  EXPECT_NEAR(under_beam.back().first, 20.15, 0.03);
  EXPECT_NEAR(under_beam.back().second, 19.90, 0.03);
  EXPECT_FALSE(sim.firing());
  EXPECT_FALSE(sim.enabled());
  EXPECT_DOUBLE_EQ(sim.output(), 0);
}

// A dragonfly that cannot run (here: a look of less than a pixel) is refused
// when the script starts it, with the beam already on: the run fails and the
// laser does not stay on.
class BlindDragonflySessionTest : public LabSessionTest {
 protected:
  void prepare_lab() override {
    std::ofstream(dir_ / "patterns" / "follow.toml", std::ios::trunc) << "kind = \"dragonfly\"\ntarget_radius = 0.001\n";
  }
};

TEST_F(BlindDragonflySessionTest, ADragonflyThatFailsLeavesTheLaserOff) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  auto& sim = laser_sim("co2");
  auto q = laser_queue();
  q.runs.at(0).extraction.pattern = "follow";
  ASSERT_TRUE(session_->start(q));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result->runs.empty());
  EXPECT_EQ(result->runs.front().state, run::RunState::Failed);
  EXPECT_NE(result->runs.front().error.value_or("").find("target_radius"), std::string::npos)
      << result->runs.front().error.value_or("");
  EXPECT_EQ(count(sim.log(), "Laser.Fire"), 1);  // it had fired
  EXPECT_FALSE(sim.firing());                    // and is off
  EXPECT_FALSE(sim.enabled());
  EXPECT_DOUBLE_EQ(sim.output(), 0);
}

// The run's duration is how long a dragonfly follows, as in legacy pychron.
TEST_F(LabSessionTest, ADragonflyRunsForTheRunsDuration) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  auto q = laser_queue();
  q.runs.erase(q.runs.begin());  // one run: hole 7
  q.runs.at(0).extraction.pattern = "follow";  // the file says 5 s
  q.runs.at(0).extraction.duration = std::chrono::seconds(12);
  const TimePoint before = clock_.now();
  ASSERT_TRUE(session_->start(q));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->runs.size(), 1u);
  EXPECT_EQ(result->runs[0].state, run::RunState::Success) << result->runs[0].error.value_or("");
  // the whole run took at least the 12 s of following (5 s would be the pattern's own)
  EXPECT_GE(clock_.now() - before, Duration(std::chrono::seconds(12)));
}

TEST_F(LabSessionTest, AnUncalibratedTrayIsRefusedAtStart) {
  ASSERT_TRUE(lab_.calibrations->clear("co2", "example-9"));
  const auto before = laser_sim("co2").log();
  const auto started = session_->start(laser_queue());
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, ErrorKind::Config);
  EXPECT_NE(started.error().what.find("example-9"), std::string::npos) << started.error().what;
  EXPECT_NE(started.error().what.find("not calibrated"), std::string::npos) << started.error().what;
  EXPECT_FALSE(session_->running());
  EXPECT_EQ(laser_sim("co2").log(), before);
}

TEST_F(LabSessionTest, ARunWhoseHoleIsOutOfTravelFailsAndTheLaserStaysOff) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  // The tray calibrated at the edge of the stage: hole 3 is then at x = 54,
  // past the 50 mm travel. The queue checks; the move is the driver's to refuse.
  const std::vector<laser::CalibrationPoint> points{{"5", 49, 25}, {"6", 54, 25}};
  ASSERT_TRUE(lab_.calibrations->save(*lab_.trays.find("example-9"), "co2", points));
  auto& sim = laser_sim("co2");
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result->runs.empty());
  EXPECT_EQ(result->runs.front().state, run::RunState::Failed);
  EXPECT_NE(result->runs.front().error.value_or("").find("hole 3"), std::string::npos)
      << result->runs.front().error.value_or("");
  EXPECT_EQ(count(sim.log(), "Laser.Fire"), 0);
  EXPECT_FALSE(sim.firing());
  EXPECT_FALSE(sim.enabled());
  EXPECT_EQ(sim.position().x, 0);
}

// The camera cannot see the hole (the tray is nowhere near where its
// calibration says). With on_failure = "fail" the run stops there: the laser
// is never fired at a place nobody could confirm.
class HiddenHoleSessionTest : public LabSessionTest {
 protected:
  virtual const char* on_failure() const { return "fail"; }
  void prepare_lab() override {
    std::ofstream(dir_ / "cameras.toml", std::ios::trunc)
        << "[co2]\n[co2.sim]\ntray_error_mm = [3.0, 3.0]\nnoise = 0\n[co2.autocenter]\non_failure = \"" << on_failure()
        << "\"\n";
  }
};

TEST_F(HiddenHoleSessionTest, AHiddenHoleFailsTheRunBeforeTheLaserFires) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  auto& sim = laser_sim("co2");
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result->runs.empty());
  EXPECT_EQ(result->runs.front().state, run::RunState::Failed);
  const std::string why = result->runs.front().error.value_or("");
  EXPECT_NE(why.find("hole 3"), std::string::npos) << why;
  EXPECT_NE(why.find("no_target"), std::string::npos) << why;
  EXPECT_EQ(count(sim.log(), "Laser.Fire"), 0);
  EXPECT_FALSE(sim.firing());
  EXPECT_FALSE(sim.enabled());
  EXPECT_EQ(sim.position().x, 30000);  // back at the calibrated position
  EXPECT_FALSE(fs::exists(dir_ / "stage_corrections" / "co2.example-9.toml"));
}

class HiddenHoleCarriedOnTest : public HiddenHoleSessionTest {
 protected:
  const char* on_failure() const override { return "continue"; }
};

// The default, as legacy pychron: carry on at the calibrated position.
TEST_F(HiddenHoleCarriedOnTest, AHiddenHoleIsCarriedOnPastByDefault) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  auto& sim = laser_sim("co2");
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->runs.size(), 2u);
  for (const auto& r : result->runs) EXPECT_EQ(r.state, run::RunState::Success) << r.error.value_or("");
  EXPECT_EQ(count(sim.log(), "Laser.Fire"), 2);
  EXPECT_FALSE(fs::exists(dir_ / "stage_corrections" / "co2.example-9.toml"));
}

// A recorded camera is for looking at what the finder sees, not for moving
// the stage: a session never closes the loop on one. Every hole is fired on
// at its calibrated position and nothing is "corrected".
class RecordedCameraSessionTest : public LabSessionTest {
 protected:
  void prepare_lab() override {
    // Pictures of a hole 0.5 mm off, as an old recording might show.
    const auto dir = dir_ / "recordings" / "holes";
    fs::create_directories(dir);
    vision::FrameRecorder recorder(dir, vision::Provenance::Synthetic, vision::FinderMode::Hole, 23);
    vision::HoleScene scene;
    scene.hole_radius_mm = 1.0;
    scene.hole_mm = {0.5, 0};
    for (int i = 0; i < 30; ++i) ASSERT_TRUE(recorder.add(vision::render(scene, {0, 0}).first.view()));
    ASSERT_TRUE(recorder.finish());
    std::ofstream(dir_ / "cameras.toml", std::ios::trunc)
        << "[co2]\nsource = \"recorded\"\nframes = \"recordings/holes\"\n";
  }
};

TEST_F(RecordedCameraSessionTest, ARecordedCameraNeverMovesTheStage) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  EXPECT_TRUE(session_->problems().empty());
  auto& sim = laser_sim("co2");
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->runs.size(), 2u);
  for (const auto& r : result->runs) EXPECT_EQ(r.state, run::RunState::Success) << r.error.value_or("");
  // the only moves at the stage's own speed are the two hole moves, to the calibrated positions
  std::vector<std::string> travel;
  for (const auto& line : sim.log()) {
    if (line.starts_with("Stage.MoveTo ") && line.ends_with(",5000,5000,100")) travel.push_back(line);
  }
  EXPECT_EQ(travel, (std::vector<std::string>{"Stage.MoveTo 30000,30000,0,5000,5000,100",
                                              "Stage.MoveTo 20000,20000,0,5000,5000,100"}));
  EXPECT_FALSE(fs::exists(dir_ / "stage_corrections" / "co2.example-9.toml"));
}

// The example's simulated camera left in a lab whose laser is real: it
// would "find" its made-up tray error and the laser would be fired there.
// The session says so and starts no queue on that device until the table is
// removed.
class SimCameraOnARealLaserTest : public LabSessionTest {
 protected:
  SessionHardware hardware() override {
    SessionHardware hw = LabSessionTest::hardware();
    hw.simulated = [](std::string_view) { return false; };  // as a real Chromium would be
    return hw;
  }
};

TEST_F(SimCameraOnARealLaserTest, ASimulatedCameraOnARealLaserStopsItsQueues) {
  ASSERT_EQ(session_->problems().size(), 1u);
  const std::string problem = session_->problems().front();
  EXPECT_NE(problem.find("simulated"), std::string::npos) << problem;
  EXPECT_NE(problem.find("co2"), std::string::npos) << problem;
  const auto before = laser_sim("co2").log();
  const auto started = session_->start(laser_queue());
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, ErrorKind::Config);
  EXPECT_EQ(started.error().what, problem);
  EXPECT_FALSE(session_->running());
  EXPECT_EQ(laser_sim("co2").log(), before);
  // a queue with no laser in it is not held up by the laser's camera
  ASSERT_TRUE(session_->start(queue_));
  session_->abort();
  session_->wait();
}

// A pattern that runs off the edge of the stage's travel: the stage refuses
// the point, the run fails there, and the laser does not stay on.
TEST_F(LabSessionTest, APatternOutOfTravelFailsTheRunAndTheLaserIsOff) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  // Hole 7 (the pattern's) at stage (0.2, 0.2): the hexagon's far side is past 0.
  const std::vector<laser::CalibrationPoint> points{{"5", 5.2, 5.2}, {"6", 10.2, 5.2}};
  ASSERT_TRUE(lab_.calibrations->save(*lab_.trays.find("example-9"), "co2", points));
  auto& sim = laser_sim("co2");
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->runs.size(), 2u);
  EXPECT_EQ(result->runs[0].state, run::RunState::Success) << result->runs[0].error.value_or("");
  EXPECT_EQ(result->runs[1].state, run::RunState::Failed);
  const std::string why = result->runs[1].error.value_or("");
  EXPECT_NE(why.find("pattern hexagon, point "), std::string::npos) << why;
  EXPECT_NE(why.find("travel"), std::string::npos) << why;
  for (const auto& line : sim.log()) {
    if (line.starts_with("Stage.MoveTo ")) EXPECT_EQ(line.find("Stage.MoveTo -"), std::string::npos) << line;
  }
  EXPECT_FALSE(sim.firing());
  EXPECT_FALSE(sim.enabled());
  EXPECT_DOUBLE_EQ(sim.output(), 0);
}

// Two lasers on one line: each run ends its own, and the first is off before
// the second run starts.
class TwoLaserSessionTest : public LabSessionTest {
 protected:
  void prepare_lab() override {
    std::ofstream(dir_ / "extraction_line.toml", std::ios::app)
        << "\n[transports.diode_pc]\nkind = \"sim\"\ntimeout_ms = 2000\n"
           "\n[drivers.diode]\nkind = \"chromium\"\ntransport = \"diode_pc\"\n";
    // The same tray, somewhere else on the diode's stage.
    std::ifstream in(dir_ / "stage_calibrations" / "co2.example-9.toml");
    std::stringstream text;
    text << in.rdbuf();
    std::string diode = text.str();
    const std::string from = "device = \"co2\"";
    diode.replace(diode.find(from), from.size(), "device = \"diode\"");
    for (const char* x : {"x = 25.0", "x = 30.0"}) {
      const auto at = diode.find(x);
      diode.replace(at, 6, x[4] == '2' ? "x = 10" : "x = 15");
    }
    std::ofstream(dir_ / "stage_calibrations" / "diode.example-9.toml") << diode;
  }
};

TEST_F(TwoLaserSessionTest, TwoDevicesInOneQueueEachEndTheirOwn) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  EXPECT_EQ(lab_.extract_devices, (std::vector<std::string>{"co2", "diode"}));
  auto& co2 = laser_sim("co2");
  auto& diode = laser_sim("diode");

  auto q = laser_queue();
  q.runs.at(1).extraction.device = "diode";

  // What the co2 laser is doing when the second run starts.
  std::atomic<int> seen{0};
  std::atomic<bool> co2_on_at_second{true};
  std::atomic<int> diode_commands_at_second{-1};
  subs_.push_back(line_->bus().subscribe<executor::RunStarted>([&](const executor::RunStarted&) {
    if (++seen != 2) return;
    co2_on_at_second = co2.firing() || co2.enabled() || co2.output() != 0;
    diode_commands_at_second = count(diode.log(), "Laser.Fire") + count(diode.log(), "Laser.Enable 1");
  }));

  ASSERT_TRUE(session_->start(q));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->runs.size(), 2u);
  for (const auto& r : result->runs) EXPECT_EQ(r.state, run::RunState::Success) << r.identifier << " " << r.error.value_or("");

  EXPECT_EQ(seen.load(), 2);
  EXPECT_FALSE(co2_on_at_second.load());
  EXPECT_EQ(diode_commands_at_second.load(), 0);
  EXPECT_EQ(count(co2.log(), "Laser.Fire"), 1);
  EXPECT_EQ(count(diode.log(), "Laser.Fire"), 1);
  EXPECT_EQ(count(co2.log(), "Stage.MoveTo 30000,30000,0,5000,5000,100"), 1);   // hole 3 on co2
  EXPECT_EQ(count(diode.log(), "Stage.MoveTo 5000,20000,0,5000,5000,100"), 1);   // hole 7 on diode
  for (auto* sim : {&co2, &diode}) {
    EXPECT_FALSE(sim->firing());
    EXPECT_FALSE(sim->enabled());
    EXPECT_DOUBLE_EQ(sim->output(), 0);
  }
}

TEST_F(LabSessionTest, AQueueThatDoesNotCheckIsRefused) {
  queue_.runs[1].measurement.plan = "no_such_plan";
  queue_.runs[2].measurement.plan = "nor_this";
  auto r = session_->start(queue_);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("2 error(s)"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("runs[1].measurement.plan"), std::string::npos) << r.error().what;
  EXPECT_FALSE(session_->running());
  EXPECT_FALSE(session_->wait().has_value());
}

TEST_F(LabSessionTest, ARunningQueueCanBeEdited) {
  EXPECT_FALSE(session_->edit(0, queue_));  // nothing running
  std::vector<std::string> errors;
  std::optional<Result<std::uint64_t>> accepted;
  // Edited as the first run starts (on the executor thread, so the rows
  // after it are not yet reached).
  auto sub = line_->bus().subscribe<executor::RunStarted>([&](const executor::RunStarted& e) {
    if (e.row != 0) return;
    auto bad = queue_;
    bad.runs[2].measurement.plan = "no_such_plan";
    if (auto r = session_->edit(0, bad); !r) errors.push_back(r.error().what);
    auto shorter = queue_;
    shorter.runs.resize(2);
    accepted = session_->edit(0, shorter);
  });
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("runs[2].measurement.plan"), std::string::npos) << errors[0];
  ASSERT_TRUE(accepted && *accepted) << (accepted && !*accepted ? accepted->error().what : "");
  EXPECT_EQ(**accepted, 1u);
  EXPECT_EQ(result->runs.size(), 2u);
}

TEST_F(LabSessionTest, TheQueueEndIsNotified) {
  lab_.notifications.commands.push_back({"log", {"notify-lab"}, {lab::NotifyEvent::QueueEnded}});
  std::mutex m;
  std::vector<std::string> inputs;
  auto runner = [&](const ProcessSpec& spec) -> Result<ProcessResult> {
    std::lock_guard lock(m);
    inputs.push_back(spec.input);
    return ProcessResult{};
  };
  std::vector<lab::NotificationSent> sent;
  auto sub = line_->bus().subscribe<lab::NotificationSent>([&](const lab::NotificationSent& e) {
    std::lock_guard lock(m);
    sent.push_back(e);
  });
  session_.reset();
  session_ = std::make_unique<LabSession>(lab_, SessionHardware{*line_, spec_.get(), scan_.get(), {}},
                                          SessionOptions{dir_ / "data", {}, runner});
  queue_.name = "q-notify";
  queue_.runs.resize(1);
  ASSERT_TRUE(session_->start(queue_));
  auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  session_->notifier().wait_idle();
  std::lock_guard lock(m);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_TRUE(sent[0].ok) << sent[0].error;
  EXPECT_EQ(sent[0].channel, "log");
  ASSERT_EQ(inputs.size(), 1u);
  EXPECT_EQ(inputs[0].rfind("pychron: queue q-notify " + std::string(executor::to_string(result->end)), 0), 0u)
      << inputs[0];
}

TEST_F(LabSessionTest, CancelEndsTheQueueAndTheSessionCanStartAgain) {
  ASSERT_TRUE(session_->start(queue_));
  ASSERT_TRUE(eventually([&] { return started_ > 0; }));
  session_->cancel();
  auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Cancelled) << result->reason;
  EXPECT_FALSE(session_->running());

  // Controls on an idle session do nothing.
  session_->stop();
  session_->abort();
  session_->truncate();

  queue_.runs.resize(1);
  ASSERT_TRUE(session_->start(queue_, 0));
  result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
}

// A QueueEnded subscriber may call back into the session while the owner
// starts the next queue (which joins the previous queue's thread).
TEST_F(LabSessionTest, AQueueEndedSubscriberMayCallBackIn) {
  std::atomic<int> called{0};
  subs_.push_back(line_->bus().subscribe<QueueEnded>([this, &called](const QueueEnded&) {
    std::this_thread::sleep_for(200ms);  // still publishing when the next start() comes
    (void)session_->running();
    (void)session_->state();
    ++called;
  }));
  queue_.runs.resize(1);
  ASSERT_TRUE(session_->start(queue_));
  ASSERT_TRUE(eventually([&] { return !session_->running(); }));
  ASSERT_TRUE(session_->start(queue_));  // joins the first thread mid-publish
  ASSERT_TRUE(session_->wait().has_value());
  EXPECT_EQ(called.load(), 2);
}

TEST_F(LabSessionTest, DestroyingARunningSessionAbortsIt) {
  ASSERT_TRUE(session_->start(queue_));
  ASSERT_TRUE(eventually([&] { return started_ > 0; }));
  session_.reset();
  ASSERT_EQ(ended().size(), 1u);
  EXPECT_EQ(ended().front().result.end, executor::QueueEnd::Aborted) << ended().front().result.reason;
}

}  // namespace
}  // namespace pychron::experiment::lab
