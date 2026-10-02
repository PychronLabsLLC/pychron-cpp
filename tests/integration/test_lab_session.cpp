// LabSession on the sim lab: a scratch copy of configs/examples run on a
// simulated clock pumped 400x, as `elctl exp run --sim --sim-speed 400` does.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "pychron/core/clock_pump.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/scripting/script_host.hpp"
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
           ("pychron-session-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_, fs::copy_options::recursive);
    fs::remove_all(dir_ / "data");

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
    session_ = std::make_unique<LabSession>(lab_, SessionHardware{*line_, spec_.get(), scan_.get()},
                                            SessionOptions{dir_ / "data", {}});
    subs_.push_back(line_->bus().subscribe<QueueEnded>([this](const QueueEnded& e) {
      std::lock_guard lock(mutex_);
      ended_.push_back(e);
    }));
    subs_.push_back(line_->bus().subscribe<executor::RunStarted>([this](const executor::RunStarted&) { ++started_; }));
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

TEST_F(LabSessionTest, DestroyingARunningSessionAbortsIt) {
  ASSERT_TRUE(session_->start(queue_));
  ASSERT_TRUE(eventually([&] { return started_ > 0; }));
  session_.reset();
  ASSERT_EQ(ended().size(), 1u);
  EXPECT_EQ(ended().front().result.end, executor::QueueEnd::Aborted) << ended().front().result.reason;
}

}  // namespace
}  // namespace pychron::experiment::lab
