// LabSession on the sim lab: a scratch copy of configs/examples run on a
// VirtualClock at unlimited speed, as `elctl exp run --sim --sim-speed max`
// does. The test's thread takes part in the clock: time moves only while it
// waits there (session.wait(), eventually()), so what a test does between two
// waits happens at one instant of the queue's time.

#include <gtest/gtest.h>

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
#include <string_view>
#include <cstdint>
#include <cstddef>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/devices/link.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/experiment/lab/lasers.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/vision/fixture.hpp"
#include "pychron/vision/synth.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "virtual_time.hpp"
#ifdef PYCHRON_EXPERIMENT_HAS_METRICS
#include "http_client.hpp"
#include "metrics_text.hpp"
#include "pychron/core/config/metrics_config.hpp"
#include "pychron/experiment/metrics/service.hpp"
#include "temp_path.hpp"
#endif

namespace pychron::experiment::lab {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// A persister that keeps no file of its own under <data>/records: it counts
// what it is asked to save and lends aliquots and the rest of FilePersister,
// on a directory of its own.
class RecordingPersister final : public persist::IAnalysisPersister {
 public:
  RecordingPersister(fs::path root, std::vector<std::string> refusals, std::optional<std::string> cannot_check = {})
      : files_(std::move(root)), refusals_(std::move(refusals)), cannot_check_(std::move(cannot_check)) {}
  // From now on save_analysis fails with this text (empty: it saves again).
  void refuse_saves(std::string why) {
    std::lock_guard lock(mutex_);
    cannot_save_ = std::move(why);
  }
  // The queues it was asked to check, in order.
  std::vector<QueueSpec> checked() {
    std::lock_guard lock(mutex_);
    return checked_;
  }
  // Numbers from 7, which an empty FilePersister never hands out first.
  Result<int> next_aliquot(const std::string& identifier) override {
    auto n = files_.next_aliquot(identifier);
    if (!n) return n;
    return *n + 6;
  }
  Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override { return files_.begin_run(id, queue); }
  Result<void> save_extraction(const record::AnalysisRecord& r) override { return files_.save_extraction(r); }
  Result<void> save_analysis(const record::AnalysisRecord& r) override {
    std::lock_guard lock(mutex_);
    if (!cannot_save_.empty()) return fail(ErrorKind::Io, cannot_save_);
    if (saved_.empty()) first_aliquot_ = r.identity.aliquot;
    saved_.push_back(r.identity.uuid);
    return {};
  }
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override {
    return files_.save_artifact(uuid, name, bytes);
  }
  Result<void> flush() override { return {}; }
  // Refuses a row that will run (not skipped) whose identifier is `zz`, as a
  // store refuses one its catalog lacks; and whatever it was told to refuse.
  Result<std::vector<std::string>> check_queue(const QueueSpec& queue) override {
    std::lock_guard lock(mutex_);
    checked_.push_back(queue);
    if (cannot_check_) return fail(ErrorKind::Io, *cannot_check_);
    std::vector<std::string> lines = refusals_;
    for (const auto& run : queue.runs)
      if (!run.skip && run.id.identifier == "zz") lines.emplace_back("identifier 'zz' is not in the database");
    return lines;
  }
  std::size_t saved() {
    std::lock_guard lock(mutex_);
    return saved_.size();
  }
  int first_aliquot() {
    std::lock_guard lock(mutex_);
    return first_aliquot_;
  }

 private:
  persist::FilePersister files_;
  std::vector<std::string> refusals_;
  std::optional<std::string> cannot_check_;
  int first_aliquot_ = 0;
  std::mutex mutex_;
  std::vector<std::string> saved_;
  std::string cannot_save_;
  std::vector<QueueSpec> checked_;
};

class LabSessionTest : public pychron::testing::VirtualTimeTest {
 protected:
  // A dragonfly's twelve seconds are some four hundred pictures made and
  // searched: under the thread sanitizer that alone is nearly twenty seconds.
  LabSessionTest() : VirtualTimeTest(50s) {}

  // Waits in the clock's time, looking every 100 ms of it, until `done`;
  // false when an hour of it has passed first.
  template <class F>
  bool eventually(F done) {
    const TimePoint until = clock_.now() + std::chrono::hours(1);
    while (!done()) {
      if (clock_.now() > until) return false;
      clock_.sleep_for(100ms);
    }
    return true;
  }

  void SetUp() override {
    dir_ = pychron::testing::unique_temp_path("pychron-session-");
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_, fs::copy_options::recursive);
    fs::remove_all(dir_ / "data");
    prepare_lab();

    systems::ExtractionLine::Options options;
    options.clock = &clock_;
    options.force_sim = true;
    options.state_file = dir_ / "line.state.toml";
    auto line = systems::ExtractionLine::load(dir_ / "extraction_line.toml", dir_ / "canvas.toml", options);
    ASSERT_TRUE(line) << line.error().what;
    line_ = std::move(*line);
    ASSERT_TRUE(line_->start());

    auto spec = spectrometer::load_spectrometer_for_app(
        dir_ / "spectrometer.sim-integrated.toml",
        spectrometer::SpectrometerContext{clock_, line_->scheduler(), line_->bus()},
        // As the applications do: the beam measures the line's gas.
        spectrometer::SpectrometerBringup{.sim_beam_from_table = true, .line_sim = line_->sim()});
    ASSERT_TRUE(spec) << spec.error().what;
    spec_ = std::move(*spec);
    scan_ = std::make_unique<spectrometer::ScanService>(*spec_, line_->bus(), clock_);

    lab_ = load_lab({dir_, dir_ / "extraction_line.toml", dir_ / "spectrometer.sim-integrated.toml"});
    if (lab_loads_clean()) {
      ASSERT_TRUE(lab_.problems.empty()) << lab_.problems.front();
    }
    auto q = load_queue_file((dir_ / "queues" / "experiment.toml").string(), lab_.ids);
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
  // False for a test that loads the lab with a file it means to be bad.
  virtual bool lab_loads_clean() { return true; }
  virtual SessionHardware hardware() { return SessionHardware{*line_, spec_.get(), scan_.get(), {}}; }

  QueueSpec laser_queue() {
    auto q = load_queue_file((dir_ / "queues" / "experiment.laser.toml").string(), lab_.ids);
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
    if (line_) line_->stop();
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
    line_.reset();
    fs::remove_all(dir_);
  }

  // For a queue aborted straight after start(), as a test does that has
  // nothing more to ask of it.
  void expect_aborted_before_any_run() {
    const auto result = session_->wait();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->end, executor::QueueEnd::Aborted) << result->reason;
    EXPECT_TRUE(result->runs.empty());
  }

  std::vector<QueueEnded> ended() {
    std::lock_guard lock(mutex_);
    return ended_;
  }

  fs::path dir_;
  VirtualClock clock_;  // before everything that is given a reference to it
  Clock::Participant test_{clock_, "test"};
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<spectrometer::Spectrometer> spec_;
  std::unique_ptr<spectrometer::ScanService> scan_;
  Lab lab_;
  QueueSpec queue_;
  std::unique_ptr<RecordingPersister> persister_;  // outlives session_
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

  // QueueEnded comes after the scan is resumed, and before wait() returns.
  ASSERT_FALSE(ended().empty());
  EXPECT_EQ(ended().front().result.end, executor::QueueEnd::Completed);
  EXPECT_TRUE(scan_->running());
  EXPECT_FALSE(scan_->paused());
}

// The session checks the links each run needs before it starts (executor
// hardening, 4.3 and 4.6): one down holds the queue, which probes it back
// and goes on.
TEST_F(LabSessionTest, ALinkTheRunNeedsIsHeldForBeforeItStarts) {
  // A spectrometer transport: nothing calls it before the first run (the
  // scan is not started here), so closed it stays down until the check.
  auto links = spec_->links();
  ASSERT_FALSE(links.empty());
  const std::string name = links.front()->transport().name();
  links.front()->transport().close();
  std::vector<executor::ExecutorHolding> held;
  std::mutex m;
  auto sub = line_->bus().subscribe<executor::ExecutorHolding>([&](const executor::ExecutorHolding& h) {
    std::lock_guard lock(m);
    held.push_back(h);
  });
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  EXPECT_EQ(result->runs.size(), 3u);
  std::lock_guard lock(m);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held[0].links, std::vector<std::string>{name});
  EXPECT_EQ(held[0].row, std::optional<std::size_t>(0));
}

// Resume and skip reach a held queue's executor; without a queue, resume
// does nothing and skip is refused.
TEST_F(LabSessionTest, AHeldRowIsSkippedThroughTheSession) {
  session_->resume();
  const auto idle = session_->skip();
  ASSERT_FALSE(idle);
  EXPECT_EQ(idle.error().kind, ErrorKind::Config);

  auto links = spec_->links();
  ASSERT_FALSE(links.empty());
  links.front()->transport().close();  // nothing reopens it before the first run's check
  std::atomic<int> held{0};
  auto sub = line_->bus().subscribe<executor::ExecutorHolding>([&](const executor::ExecutorHolding& h) {
    if (h.row == std::optional<std::size_t>(0) && held.fetch_add(1) == 0) {
      EXPECT_TRUE(session_->skip());
    }
  });
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  EXPECT_EQ(held.load(), 1);
  // Row 0 dropped before it started: the other two ran.
  ASSERT_EQ(result->runs.size(), 2u);
  EXPECT_EQ(result->runs[0].row, 1u);
}

TEST_F(LabSessionTest, AQueueThePersisterRefusesIsNotStarted) {
  session_.reset();
  persister_ = std::make_unique<RecordingPersister>(dir_ / "elsewhere",
                                                    std::vector<std::string>{"identifier 'zz' is not in the database"});
  session_ = std::make_unique<LabSession>(lab_, hardware(), SessionOptions{dir_ / "data", {}, {}, persister_.get()});
  std::atomic<int> announced{0};
  auto sub = line_->bus().subscribe<QueueStarted>([&](const QueueStarted&) { ++announced; });

  auto refused = session_->start(queue_);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  EXPECT_NE(refused.error().what.find("the queue cannot be saved:"), std::string::npos) << refused.error().what;
  EXPECT_NE(refused.error().what.find("identifier 'zz' is not in the database"), std::string::npos);
  EXPECT_FALSE(session_->running());
  clock_.sleep_for(1s);
  EXPECT_EQ(announced.load(), 0);
}

TEST_F(LabSessionTest, AQueueIsNotStartedWhenThePersisterCannotCheckIt) {
  session_.reset();
  persister_ = std::make_unique<RecordingPersister>(dir_ / "elsewhere", std::vector<std::string>{},
                                                    std::optional<std::string>("db down"));
  session_ = std::make_unique<LabSession>(lab_, hardware(), SessionOptions{dir_ / "data", {}, {}, persister_.get()});
  auto refused = session_->start(queue_);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().what, "the queue cannot be saved: db down");
  EXPECT_FALSE(session_->running());
}

TEST_F(LabSessionTest, RunsAreSavedThroughTheGivenPersister) {
  session_.reset();
  persister_ = std::make_unique<RecordingPersister>(dir_ / "elsewhere", std::vector<std::string>{});
  session_ = std::make_unique<LabSession>(lab_, hardware(), SessionOptions{dir_ / "data", {}, {}, persister_.get()});
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  EXPECT_EQ(persister_->saved(), 3u);
  EXPECT_EQ(persister_->first_aliquot(), 7);
  EXPECT_FALSE(fs::exists(dir_ / "data" / "records" / "bu" / "bu-1.json"));
  EXPECT_TRUE(fs::is_empty(dir_ / "data" / "spool"));
  EXPECT_EQ(session_->pending_saves(), 0u);
}

// A session told that its hardware is a simulator's marks every record it
// makes; one that is not told leaves them as they always were.
TEST_F(LabSessionTest, ASimulatedSessionMarksItsRecords) {
  const auto simulated_of = [&](const fs::path& data) {
    std::vector<bool> out;
    for (const auto& dir : fs::directory_iterator(data / "records")) {
      if (!dir.is_directory()) continue;
      for (const auto& f : fs::directory_iterator(dir.path())) {
        const std::string name = f.path().filename().string();
        if (!name.ends_with(".json") || name.ends_with(".extraction.json")) continue;
        std::ifstream in(f.path());
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto rec = record::from_json(text);
        EXPECT_TRUE(rec.has_value());
        if (rec) out.push_back(rec->instrument.simulated);
      }
    }
    return out;
  };
  queue_.runs.resize(1);
  ASSERT_TRUE(session_->start(queue_));
  ASSERT_TRUE(session_->wait().has_value());
  EXPECT_EQ(simulated_of(dir_ / "data"), std::vector<bool>{false});

  session_.reset();
  auto hw = hardware();
  hw.simulation = true;
  session_ = std::make_unique<LabSession>(lab_, hw, SessionOptions{dir_ / "sim-data", {}, {}});
  ASSERT_TRUE(session_->start(queue_));
  ASSERT_TRUE(session_->wait().has_value());
  EXPECT_EQ(simulated_of(dir_ / "sim-data"), std::vector<bool>{true});
}

// The check is of what will run. Row 0 names something the database lacks:
// from the top the queue is refused, from row 1 it starts, and the persister
// is asked about a queue whose earlier rows are marked as not running.
TEST_F(LabSessionTest, AQueueIsCheckedFromTheRowItStartsAt) {
  session_.reset();
  persister_ = std::make_unique<RecordingPersister>(dir_ / "elsewhere", std::vector<std::string>{});
  session_ = std::make_unique<LabSession>(lab_, hardware(), SessionOptions{dir_ / "data", {}, {}, persister_.get()});
  queue_.runs[0] = queue_.runs[1];  // an unknown, so that any identifier will do for the lab
  queue_.runs[0].id.identifier = "zz";

  auto refused = session_->start(queue_, 0);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().what, "the queue cannot be saved:\nidentifier 'zz' is not in the database");

  ASSERT_TRUE(session_->start(queue_, 1));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  ASSERT_EQ(result->runs.size(), 2u);
  EXPECT_EQ(result->runs[0].row, 1u);
  EXPECT_EQ(persister_->saved(), 2u);

  const auto checked = persister_->checked();
  ASSERT_EQ(checked.size(), 2u);
  EXPECT_FALSE(checked[0].runs[0].skip);
  ASSERT_EQ(checked[1].runs.size(), 3u);
  EXPECT_TRUE(checked[1].runs[0].skip);  // before the start row
  EXPECT_FALSE(checked[1].runs[1].skip);
  EXPECT_FALSE(checked[1].runs[2].skip);
  EXPECT_EQ(checked[1].mass_spectrometer, queue_.mass_spectrometer);
}

// A row added while the queue runs is checked like the rows it started with:
// one the database cannot take is refused then, not measured and kept in the
// spool. Rows the executor has reached are not asked about again.
TEST_F(LabSessionTest, AnEditThatCannotBeSavedIsRefused) {
  session_.reset();
  persister_ = std::make_unique<RecordingPersister>(dir_ / "elsewhere", std::vector<std::string>{});
  session_ = std::make_unique<LabSession>(lab_, hardware(), SessionOptions{dir_ / "data", {}, {}, persister_.get()});
  std::vector<std::string> errors;
  std::optional<Result<std::uint64_t>> accepted;
  auto sub = line_->bus().subscribe<executor::RunStarted>([&](const executor::RunStarted& e) {
    if (e.row != 0) return;
    auto longer = queue_;
    longer.runs.push_back(queue_.runs[2]);
    longer.runs.back().id.identifier = "zz";
    if (auto r = session_->edit(0, longer); !r) errors.push_back(r.error().what);
    // Queue-level fields of an edit are ignored: the running queue's are the ones checked.
    auto fine = queue_;
    fine.mass_spectrometer = "not-the-running-queue's";
    fine.runs.push_back(queue_.runs[2]);
    accepted = session_->edit(0, fine);
  });
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0], "the queue cannot be saved:\nidentifier 'zz' is not in the database");
  ASSERT_TRUE(accepted && *accepted) << (accepted && !*accepted ? accepted->error().what : "");
  EXPECT_EQ(result->runs.size(), 4u);
  EXPECT_EQ(persister_->saved(), 4u);

  const auto checked = persister_->checked();
  ASSERT_EQ(checked.size(), 3u);  // the start, and the two edits
  ASSERT_EQ(checked[1].runs.size(), 4u);
  EXPECT_TRUE(checked[1].runs[0].skip);  // reached: it is running
  EXPECT_FALSE(checked[1].runs[3].skip);
  EXPECT_EQ(checked[2].mass_spectrometer, queue_.mass_spectrometer);
}

// The store is away for the whole queue: every run is measured and kept in
// the spool, each says so and why, each is flagged (which is what the queue
// table, the notification and the save-error counter go by), and the queue
// goes on to its end. When the store is back the next start sends them.
TEST_F(LabSessionTest, ARunTheStoreDidNotTakeIsFlaggedSaysWhyAndTheQueueGoesOn) {
  lab_.notifications.commands.push_back({"log", {"notify-lab"}, {lab::NotifyEvent::RunFailed}});
  std::mutex m;
  std::vector<std::string> inputs;
  auto runner = [&](const ProcessSpec& spec) -> Result<ProcessResult> {
    std::lock_guard lock(m);
    inputs.push_back(spec.input);
    return ProcessResult{};
  };
  session_.reset();
  persister_ = std::make_unique<RecordingPersister>(dir_ / "elsewhere", std::vector<std::string>{});
  persister_->refuse_saves("the database is locked");
  session_ = std::make_unique<LabSession>(lab_, hardware(), SessionOptions{dir_ / "data", {}, runner, persister_.get()});
  std::vector<run::RunNote> notes;
  auto sub = line_->bus().subscribe<run::RunNote>([&](const run::RunNote& n) {
    std::lock_guard lock(m);
    notes.push_back(n);
  });
  EXPECT_FALSE(session_->last_save_error().has_value());

  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  ASSERT_EQ(result->runs.size(), 3u);
  const std::string why = "not saved to the data store, kept in the spool: the database is locked";
  for (const auto& r : result->runs) {
    EXPECT_EQ(r.state, run::RunState::Success) << r.identifier << " " << r.error.value_or("");
    EXPECT_TRUE(r.save_error) << r.identifier;
    EXPECT_FALSE(r.error.has_value()) << r.identifier;
    EXPECT_EQ(std::count(r.messages.begin(), r.messages.end(), why), 1) << r.identifier;
  }
  EXPECT_EQ(persister_->saved(), 0u);
  EXPECT_EQ(session_->pending_saves(), 3u);
  ASSERT_TRUE(session_->last_save_error().has_value());
  EXPECT_NE(session_->last_save_error()->find("the database is locked"), std::string::npos);
  {
    std::lock_guard lock(m);
    EXPECT_EQ(std::count_if(notes.begin(), notes.end(), [&](const run::RunNote& n) { return n.text == why; }), 3);
  }
  {
    const Clock::Detached detached(clock_);
    session_->notifier().wait_idle();
  }
  {
    std::lock_guard lock(m);
    ASSERT_EQ(inputs.size(), 3u);  // one message a run, as for a run that failed
    EXPECT_NE(inputs[0].find("not saved (queue"), std::string::npos) << inputs[0];
    EXPECT_NE(inputs[0].find("Reason:  the database is locked"), std::string::npos) << inputs[0];
  }

  // The store is back: the next queue's start sends what was kept.
  persister_->refuse_saves("");
  queue_.runs.resize(1);
  ASSERT_TRUE(session_->start(queue_));
  const auto again = session_->wait();
  ASSERT_TRUE(again.has_value());
  ASSERT_EQ(again->runs.size(), 1u);
  EXPECT_FALSE(again->runs[0].save_error);
  EXPECT_EQ(persister_->saved(), 4u);
  EXPECT_EQ(session_->pending_saves(), 0u);
  EXPECT_FALSE(session_->last_save_error().has_value());
}

// A subscriber learns the size of the queue before any of its runs starts
// (the metrics' queue progress depends on it).
TEST_F(LabSessionTest, TheQueuesStartIsAnnouncedBeforeItsFirstRun) {
  std::mutex m;
  std::vector<std::string> order;
  std::vector<QueueStarted> started;
  auto s1 = line_->bus().subscribe<QueueStarted>([&](const QueueStarted& e) {
    std::lock_guard lock(m);
    order.emplace_back("queue");
    started.push_back(e);
  });
  auto s2 = line_->bus().subscribe<executor::RunStarted>([&](const executor::RunStarted&) {
    std::lock_guard lock(m);
    order.emplace_back("run");
  });
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());

  std::lock_guard lock(m);
  ASSERT_EQ(started.size(), 1u);
  EXPECT_EQ(started.front().rows, queue_.runs.size());
  EXPECT_EQ(started.front().from_row, 0u);
  ASSERT_FALSE(order.empty());
  EXPECT_EQ(order.front(), "queue");
  EXPECT_EQ(order.size(), 1u + result->runs.size());
}

// A running queue holds the field table (field table editor spec, section 5):
// the editor's Save and Make Current are refused while it runs, and free
// again once the queue has ended.
TEST_F(LabSessionTest, TheQueueHoldsTheFieldTableWhileItRuns) {
  std::mutex m;
  std::vector<std::optional<std::string>> held;
  auto sub = line_->bus().subscribe<executor::RunStarted>([&](const executor::RunStarted&) {
    const auto who = spec_->table_held();
    std::lock_guard lock(m);
    held.push_back(who);
  });
  ASSERT_FALSE(spec_->table_held().has_value());
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  ASSERT_FALSE(ended().empty());

  std::lock_guard lock(m);
  ASSERT_FALSE(held.empty());
  for (const auto& who : held) EXPECT_EQ(who, std::optional<std::string>("queue"));
  EXPECT_FALSE(spec_->table_held().has_value());
}

// --- the metrics endpoint over a whole queue (observability design, 6) --------

#ifdef PYCHRON_EXPERIMENT_HAS_METRICS

// The session's line with `[metrics]` on, at a loopback port the OS picks.
class MetricsSessionTest : public LabSessionTest {
 protected:
  void SetUp() override {
    LabSessionTest::SetUp();
    config::MetricsConfig c;
    c.enabled = true;
    c.bind = "127.0.0.1";
    c.port = 0;
    metrics_ = metrics::MetricsService::start(c, line_->bus(), line_->scheduler(), clock_, line_->log_hub(), "0.0.0");
    ASSERT_NE(metrics_, nullptr);
    ASSERT_TRUE(metrics_->listening());
    subs_.push_back(line_->bus().subscribe<executor::RunStarted>([this](const executor::RunStarted& e) {
      std::lock_guard lock(mutex_);
      named_.push_back(e.run_id);
      named_.push_back(e.identifier);
    }));
  }
  // As the application does: the line is stopped (nothing publishes, no job
  // runs) before the service goes, and the service before the line.
  void TearDown() override {
    subs_.clear();
    session_.reset();
    scan_.reset();
    if (line_) line_->stop();
    metrics_.reset();
    LabSessionTest::TearDown();
  }

  // What the lab's Prometheus would read now.
  std::string scrape() { return http_client::body(http_client::get(metrics_->port(), "/metrics")); }

  std::unique_ptr<metrics::MetricsService> metrics_;
  std::vector<std::string> named_;  // every run id and identifier the queue used
};

TEST_F(MetricsSessionTest, ASimulatedQueueIsCountedAndNamesNoRun) {
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  const auto runs = static_cast<double>(result->runs.size());
  ASSERT_GT(runs, 0);

  const std::string body = scrape();
  using metrics_text::value;
  EXPECT_DOUBLE_EQ(value(body, "pychron_runs_started_total"), runs);
  EXPECT_DOUBLE_EQ(value(body, "pychron_runs_finished_total{state=\"success\",truncated=\"false\"}"), runs);
  EXPECT_DOUBLE_EQ(value(body, "pychron_queues_ended_total{end=\"completed\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(body, "pychron_queue_active"), 0.0);
  EXPECT_DOUBLE_EQ(value(body, "pychron_executor_state{state=\"idle\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(body, "pychron_queue_runs{status=\"total\"}"), runs);
  EXPECT_DOUBLE_EQ(value(body, "pychron_queue_runs{status=\"done\"}"), runs);
  EXPECT_DOUBLE_EQ(value(body, "pychron_run_save_errors_total"), 0.0);
  // Every run went through each phase once, and the phases took simulated time.
  for (const char* state : {"preparing", "extracting", "equilibrating", "measuring", "post_measuring", "saving"}) {
    EXPECT_DOUBLE_EQ(value(body, std::string("pychron_run_state_duration_seconds_count{state=\"") + state + "\"}"), runs)
        << state;
  }
  EXPECT_GT(value(body, "pychron_run_state_duration_seconds_sum{state=\"measuring\"}"), 1.0);
  EXPECT_GT(value(body, "pychron_measurement_blocks_total{block=\"main\",ok=\"true\"}"), 0.0);
  EXPECT_TRUE(metrics_text::has(body, "pychron_last_run_finished_age_seconds"));
  EXPECT_GE(value(body, "pychron_scheduler_heartbeat_age_seconds"), 0.0);
  EXPECT_LT(value(body, "pychron_scheduler_heartbeat_age_seconds"), 60.0);
  // The line was read while the queue ran.
  EXPECT_NE(body.find("\npychron_pressure{"), std::string::npos);
  EXPECT_GT(value(body, "pychron_scheduler_job_runs_total{job=\"metrics.heartbeat\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(body, "pychron_metrics_dropped_series_total"), 0.0);

  // No run is named: neither its id nor its sample's identifier.
  std::lock_guard lock(mutex_);
  ASSERT_FALSE(named_.empty());
  for (const std::string& name : named_) {
    ASSERT_FALSE(name.empty());
    // As a label's whole value: an identifier may be as short as "bu".
    EXPECT_EQ(body.find("\"" + name + "\""), std::string::npos) << "the scrape names " << name;
  }
}

// Every link of the line and of the spectrometer is counted from zero, by
// its transport's name, before it has reconnected or repeated anything.
TEST_F(MetricsSessionTest, EveryLinkIsCountedFromZero) {
  std::vector<Link*> links = line_->links();
  if (spec_) {
    const std::vector<Link*> more = spec_->links();
    links.insert(links.end(), more.begin(), more.end());
  }
  ASSERT_FALSE(links.empty());
  metrics_->watch_links(links);
  const std::string body = scrape();
  for (const Link* l : links) {
    const std::string labels = "{transport=\"" + l->transport().name() + "\"}";
    EXPECT_TRUE(metrics_text::has(body, "pychron_link_reconnects_total" + labels)) << labels;
    EXPECT_DOUBLE_EQ(metrics_text::value(body, "pychron_link_reconnects_total" + labels), 0.0) << labels;
    EXPECT_TRUE(metrics_text::has(body, "pychron_link_repeats_total" + labels)) << labels;
    EXPECT_DOUBLE_EQ(metrics_text::value(body, "pychron_link_repeats_total" + labels), 0.0) << labels;
  }
}

TEST_F(MetricsSessionTest, ScrapingDuringAQueueDoesNotDisturbIt) {
  std::atomic<bool> stop{false};
  std::atomic<int> scrapes{0}, bad{0};
  std::thread scraper([&] {
    while (!stop) {
      const std::string response = http_client::get(metrics_->port(), "/metrics");
      ++scrapes;
      if (!response.starts_with("HTTP/1.1 200")) ++bad;
    }
  });
  ASSERT_TRUE(session_->start(queue_));
  const auto result = session_->wait();
  stop = true;
  scraper.join();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  for (const auto& r : result->runs) EXPECT_EQ(r.state, run::RunState::Success) << r.identifier;
  EXPECT_GT(scrapes.load(), 0);
  EXPECT_EQ(bad.load(), 0);
}

#endif  // PYCHRON_EXPERIMENT_HAS_METRICS

// --- lasers shared with whoever else drives them (laser window design, 2) -----

// The session is given the lab's lasers instead of building its own.
class SharedLasersSessionTest : public LabSessionTest {
 protected:
  SessionHardware hardware() override {
    lasers_ = std::make_unique<Lasers>(lab_, *line_);
    return SessionHardware{*line_, spec_.get(), scan_.get(), {}, lasers_.get()};
  }
  void TearDown() override {
    session_.reset();
    lasers_.reset();
    LabSessionTest::TearDown();
  }
  std::unique_ptr<Lasers> lasers_;
};

TEST_F(SharedLasersSessionTest, ThereIsOneLaserSystemPerExtractionDriver) {
  EXPECT_EQ(lasers_->names(), (std::vector<std::string>{"co2"}));
  ASSERT_NE(lasers_->find("co2"), nullptr);
  EXPECT_EQ(lasers_->find("co2")->device_name(), "co2");
  EXPECT_TRUE(lasers_->find("co2")->has_camera()) << "the example lab gives it one";
  EXPECT_EQ(lasers_->find("bone_gauge"), nullptr) << "not an extraction device";
  EXPECT_TRUE(lasers_->problems().empty());
  EXPECT_EQ(lasers_->problem_of("co2"), nullptr);
  EXPECT_EQ(session_->problems(), lasers_->problems());
}

TEST_F(SharedLasersSessionTest, ACameraThatCannotBeUsedHereIsAProblem) {
  // the same lab over a laser that is not simulated: its simulated camera may not move that stage
  Lasers real(lab_, *line_, [](std::string_view) { return false; });
  ASSERT_NE(real.problem_of("co2"), nullptr);
  EXPECT_EQ(real.problems().size(), 1u);
  EXPECT_FALSE(real.find("co2")->has_camera());
}

TEST_F(SharedLasersSessionTest, OnlyOneDrivesAtATime) {
  EXPECT_EQ(lasers_->driver(), Lasers::Driver::None);
  {
    auto hand = lasers_->drive(Lasers::Driver::Manual);
    ASSERT_TRUE(hand) << hand.error().what;
    EXPECT_EQ(lasers_->driver(), Lasers::Driver::Manual);
    const auto queue = lasers_->drive(Lasers::Driver::Queue);
    ASSERT_FALSE(queue);
    EXPECT_NE(queue.error().what.find("by hand"), std::string::npos) << queue.error().what;
    // by hand again is the same driver
    auto again = lasers_->drive(Lasers::Driver::Manual);
    ASSERT_TRUE(again);
    again->release();
    EXPECT_EQ(lasers_->driver(), Lasers::Driver::Manual) << "the first still holds it";
  }
  EXPECT_EQ(lasers_->driver(), Lasers::Driver::None);
  auto queue = lasers_->drive(Lasers::Driver::Queue);
  ASSERT_TRUE(queue);
  const auto hand = lasers_->drive(Lasers::Driver::Manual);
  ASSERT_FALSE(hand);
  EXPECT_NE(hand.error().what.find("a queue is running"), std::string::npos) << hand.error().what;
  EXPECT_FALSE(lasers_->drive(Lasers::Driver::Queue));
  Lasers::Lease moved = std::move(*queue);
  EXPECT_EQ(lasers_->driver(), Lasers::Driver::Queue);
  moved.release();
  EXPECT_EQ(lasers_->driver(), Lasers::Driver::None);
}

TEST_F(SharedLasersSessionTest, AQueueIsNotStartedWhileTheLaserIsDrivenByHand) {
  auto hand = lasers_->drive(Lasers::Driver::Manual);
  ASSERT_TRUE(hand);
  const auto started = session_->start(queue_);
  ASSERT_FALSE(started);
  EXPECT_NE(started.error().what.find("by hand"), std::string::npos) << started.error().what;
  EXPECT_FALSE(session_->running());
  hand->release();
  ASSERT_TRUE(session_->start(queue_));
  session_->abort();
  expect_aborted_before_any_run();
}

TEST_F(SharedLasersSessionTest, AQueueIsNotStartedUntilAnEmergencyStopIsReset) {
  ASSERT_TRUE(lasers_->find("co2")->emergency_stop());
  EXPECT_EQ(lasers_->stopped(), (std::vector<std::string>{"co2"}));
  const auto started = session_->start(queue_);
  ASSERT_FALSE(started);
  EXPECT_NE(started.error().what.find("co2: emergency stop not reset"), std::string::npos) << started.error().what;
  EXPECT_EQ(lasers_->driver(), Lasers::Driver::None) << "a queue that did not start holds nothing";
  lasers_->find("co2")->reset_stop();
  ASSERT_TRUE(session_->start(queue_));
  session_->abort();
  expect_aborted_before_any_run();
}

// A beam somebody opened by hand has no owner once that command is over: a
// queue is not started over it (the window could then no longer close it).
TEST_F(SharedLasersSessionTest, AQueueIsNotStartedWhileABeamIsOn) {
  laser::LaserSystem& co2 = *lasers_->find("co2");
  ASSERT_TRUE(co2.enable());
  ASSERT_TRUE(co2.extract(10, extraction::ExtractUnits::Percent));
  ASSERT_TRUE(co2.fire_laser());
  EXPECT_EQ(lasers_->firing(), (std::vector<std::string>{"co2"}));
  const auto started = session_->start(queue_);
  ASSERT_FALSE(started);
  EXPECT_NE(started.error().what.find("co2: the laser is firing"), std::string::npos) << started.error().what;
  EXPECT_EQ(lasers_->driver(), Lasers::Driver::None);
  EXPECT_TRUE(laser_sim("co2").firing()) << "refusing changes nothing";
  ASSERT_TRUE(co2.end_extract());
  EXPECT_TRUE(lasers_->firing().empty());
  ASSERT_TRUE(session_->start(queue_));
  session_->abort();
  expect_aborted_before_any_run();
}

TEST_F(SharedLasersSessionTest, TheLasersAreTheQueuesWhileItRuns) {
  ASSERT_TRUE(session_->start(queue_));
  EXPECT_EQ(lasers_->driver(), Lasers::Driver::Queue);
  EXPECT_FALSE(lasers_->drive(Lasers::Driver::Manual));
  session_->abort();
  expect_aborted_before_any_run();
  // given back by the time anyone is told the queue has ended
  ASSERT_FALSE(ended().empty());
  EXPECT_EQ(lasers_->driver(), Lasers::Driver::None);
  EXPECT_TRUE(lasers_->drive(Lasers::Driver::Manual));
}

TEST_F(SharedLasersSessionTest, AQueueDrivesTheLasersItWasGiven) {
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  // what the window would watch is what the queue drove
  const laser::LaserSnapshot seen = lasers_->find("co2")->snapshot();
  EXPECT_EQ(seen.tray, "example-9");
  EXPECT_EQ(seen.autocenter.result, laser::AutocenterOutcome::Result::Converged);
  EXPECT_EQ(seen.firing, false);
}

// --- cameras for looking, and live ones (live camera design, section 3) -------

class CameraLabTest : public SharedLasersSessionTest {
 protected:
  void prepare_lab() override { std::ofstream(dir_ / "cameras.toml", std::ios::trunc) << cameras(); }
  virtual std::string cameras() const = 0;
};

class ViewCameraTest : public CameraLabTest {
  std::string cameras() const override { return "[co2]\nuse = \"view\"\n[co2.sim]\ntray_error_mm = [0.15, -0.10]\nnoise = 0\n"; }
};

TEST_F(ViewCameraTest, ACameraForLookingShowsAPictureAndStopsNoQueue) {
  EXPECT_TRUE(lasers_->problems().empty());
  EXPECT_TRUE(lasers_->notes().empty());
  laser::LaserSystem& co2 = *lasers_->find("co2");
  EXPECT_TRUE(co2.has_camera());
  EXPECT_FALSE(co2.can_center());
  EXPECT_TRUE(co2.view());
  if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run laser_extract.py";
  ASSERT_TRUE(session_->start(laser_queue()));
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  // the holes were gone to as calibrated: nothing the camera saw moved the stage
  EXPECT_EQ(co2.last_autocenter().result, laser::AutocenterOutcome::Result::None);
  for (const auto& line : laser_sim("co2").log()) {
    // (the second run also traces a hexagon about its hole: those moves are within a millimetre of it)
    if (line.starts_with("Stage.MoveTo 30")) {
      EXPECT_TRUE(line.starts_with("Stage.MoveTo 30000,30000,")) << line;
    }
    EXPECT_NE(line, "Stage.MoveTo 30150,29900,0,5000,5000,100") << "centered, by a camera that is only for looking";
  }
}

// A scale somebody measured is used instead of what cameras.toml says.
class MeasuredScaleTest : public CameraLabTest {
  std::string cameras() const override { return "[co2]\npx_per_mm = 23\n"; }
  void prepare_lab() override {
    CameraLabTest::prepare_lab();
    fs::create_directories(dir_ / "camera_scales");
    std::ofstream(dir_ / "camera_scales" / "co2.toml") << "m = [[0.05, 0.0], [0.0, -0.05]]\nresidual_mm = 0.0\n";
  }
};

TEST_F(MeasuredScaleTest, TheLabsMeasuredScaleIsUsed) {
  EXPECT_TRUE(lasers_->problems().empty());
  const auto seen = lasers_->find("co2")->view();
  ASSERT_TRUE(seen) << seen.error().what;
  EXPECT_NEAR(seen->px_per_mm, 20, 1e-9);
}

// A scale measured with the camera set up another way is not this camera's.
class ScaleOfAnotherSetupTest : public CameraLabTest {
  std::string cameras() const override { return "[co2]\n[co2.sim]\nwidth = 320\n"; }
  void prepare_lab() override {
    CameraLabTest::prepare_lab();
    fs::create_directories(dir_ / "camera_scales");
    std::ofstream(dir_ / "camera_scales" / "co2.toml")
        << "geometry = \"sim 200x200\"\nm = [[0.05, 0.0], [0.0, -0.05]]\nresidual_mm = 0.0\n";
  }
};

TEST_F(ScaleOfAnotherSetupTest, IsAProblemUntilMeasuredAgainOrCleared) {
  ASSERT_EQ(lasers_->problems().size(), 1u);
  const std::string problem = lasers_->problems().front();
  EXPECT_NE(problem.find("camera-scale"), std::string::npos) << problem;
  EXPECT_FALSE(lasers_->find("co2")->can_center());
}

class BrokenScaleTest : public CameraLabTest {
  std::string cameras() const override { return "[co2]\n"; }
  void prepare_lab() override {
    CameraLabTest::prepare_lab();
    fs::create_directories(dir_ / "camera_scales");
    std::ofstream(dir_ / "camera_scales" / "co2.toml") << "m = [[0.05]]\n";
  }
};

// A camera that centers holes by a scale nobody can read is not used.
TEST_F(BrokenScaleTest, AScaleFileThatCannotBeReadIsAProblem) {
  ASSERT_EQ(lasers_->problems().size(), 1u);
  EXPECT_NE(lasers_->problems().front().find("camera_scales"), std::string::npos) << lasers_->problems().front();
  EXPECT_FALSE(lasers_->find("co2")->can_center());
}

class LiveCameraOnASimulatedLaserTest : public CameraLabTest {
  std::string cameras() const override { return "[co2]\nsource = \"opencv\"\n"; }
};

TEST_F(LiveCameraOnASimulatedLaserTest, IsAProblemThatSaysWhatToDo) {
  ASSERT_EQ(lasers_->problems().size(), 1u);
  EXPECT_NE(lasers_->problems().front().find("use = \"view\""), std::string::npos) << lasers_->problems().front();
  EXPECT_FALSE(lasers_->find("co2")->has_camera()) << "and no camera was opened to find that out";
  EXPECT_FALSE(session_->start(laser_queue()));
}

class LiveViewCameraThatIsNotThereTest : public CameraLabTest {
  std::string cameras() const override {
    return "[co2]\nsource = \"opencv\"\nuse = \"view\"\n[co2.opencv]\ndevice = \"no/such/clip.mp4\"\ntimeout_ms = 200\n";
  }
};

TEST_F(LiveViewCameraThatIsNotThereTest, IsANoteAndStopsNoQueue) {
  EXPECT_TRUE(lasers_->problems().empty());
  ASSERT_EQ(lasers_->notes().size(), 1u);
  EXPECT_NE(lasers_->notes().front().find("co2"), std::string::npos) << lasers_->notes().front();
  ASSERT_TRUE(session_->start(queue_));
  session_->abort();
  expect_aborted_before_any_run();
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

  // example-9 is calibrated with its center at stage (25, 25): hole 3 at
  // (30, 30) mm, hole 7 at (20, 20). The example's camera (cameras.toml) sees
  // the tray 0.15, -0.10 mm from there, and each hole move ends by centering
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
  // beam on, the stage goes round it about the centered hole and comes back.
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

// Whether each hole was centered is said as it happens, is in the run's
// summary and is kept in its record.
TEST_F(LabSessionTest, ALaserRunSaysWhetherItsHoleWasCentered) {
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
  const char* holes[] = {"hole 3: centered, moved ", "hole 7: centered, moved "};
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& r = result->runs[i];
    ASSERT_EQ(r.state, run::RunState::Success) << r.error.value_or("");
    const auto centered = [&](const std::vector<std::string>& lines) {
      return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.starts_with(holes[i]); });
    };
    EXPECT_TRUE(centered(r.messages)) << ::testing::PrintToString(r.messages);
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
    EXPECT_TRUE(centered(notes)) << ::testing::PrintToString(notes);
  }
}

// A run whose pattern is a dragonfly: with the beam on, the stage leaves the
// centered hole for the glowing grain (the example's camera has it 0.2, 0.1 mm
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
  // hole 7 really at (20.15, 19.90); the grain 0.2, 0.1 from its center
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

// A plan that names a peak center config the lab file lacks at load time. The
// session reads peak_center.toml again when a queue starts, so a set saved
// meanwhile (the spectrometer window's) is the one the plan gets.
class NamedPeakCenterSessionTest : public LabSessionTest {
 protected:
  void prepare_lab() override {
    const fs::path plan = dir_ / "plans" / "sim_multicollect.toml";
    std::ifstream in(plan);
    std::stringstream text;
    text << in.rdbuf();
    std::string s = text.str();
    const auto at = s.find("config = \"default\"");
    ASSERT_NE(at, std::string::npos);
    s.replace(at, std::string_view("config = \"default\"").size(), "config = \"tuned\"");
    std::ofstream(plan, std::ios::trunc) << s;
  }
  // Added to the lab's file, or in its place.
  void write_peak_centers(std::string_view text, bool replace = false) {
    std::ofstream(dir_ / "peak_center.toml", replace ? std::ios::trunc : std::ios::app) << text;
  }
};

TEST_F(NamedPeakCenterSessionTest, ASetAddedAfterLoadIsUsedAtTheNextStart) {
  write_peak_centers("\n[tuned]\nisotope = \"Ar40\"\ndetector = \"H1\"\nwindow = 0.07\nstep = 0.002\n"
                     "integration_s = 1.0\nn_tries = 1\nupdate_table = false\n");
  std::mutex m;
  std::vector<jobs::PeakCenterConfig> used;
  auto sub = line_->bus().subscribe<jobs::PeakCenterDone>([&](const jobs::PeakCenterDone& d) {
    std::lock_guard lock(m);
    used.push_back(d.result.config);
  });
  const auto started = session_->start(queue_);
  ASSERT_TRUE(started) << started.error().what;
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  std::lock_guard lock(m);
  ASSERT_FALSE(used.empty());
  for (const auto& c : used) {
    EXPECT_EQ(c.name, "tuned");
    EXPECT_DOUBLE_EQ(c.window, 0.07);
  }
}

TEST_F(NamedPeakCenterSessionTest, ABadPeakCenterFileRefusesTheStart) {
  write_peak_centers("\n[x]\nwindow = -1\n");
  const auto started = session_->start(queue_);
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, ErrorKind::Config);
  EXPECT_NE(started.error().what.find("peak_center.toml"), std::string::npos) << started.error().what;
  // The file's own error, not the plan's missing set.
  EXPECT_EQ(started.error().what.find("uses peak center config"), std::string::npos) << started.error().what;
  EXPECT_FALSE(session_->running());

  // Put right, the file starts the queue; nothing of the bad one is kept.
  write_peak_centers("\n[tuned]\nisotope = \"Ar40\"\ndetector = \"H1\"\nwindow = 0.07\nstep = 0.002\n"
                     "integration_s = 1.0\nn_tries = 1\nupdate_table = false\n", true);
  const auto again = session_->start(queue_);
  ASSERT_TRUE(again) << again.error().what;
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
}

// The check a window shows before Start (LabSession::check_queue) reads the
// file as start() will: a set saved after the lab was loaded makes the queue
// good without a restart.
TEST_F(NamedPeakCenterSessionTest, TheCheckReadsTheFileAsItIsNow) {
  const auto before = session_->check_queue(queue_);
  ASSERT_FALSE(before.ok());
  bool missing = false;
  for (const auto& d : before.all()) missing = missing || d.message.find("uses peak center config 'tuned'") != std::string::npos;
  EXPECT_TRUE(missing);

  write_peak_centers("\n[tuned]\nisotope = \"Ar40\"\ndetector = \"H1\"\nwindow = 0.07\nstep = 0.002\n"
                     "integration_s = 1.0\nn_tries = 1\nupdate_table = false\n");
  const auto after = session_->check_queue(queue_);
  EXPECT_TRUE(after.ok()) << (after.all().empty() ? std::string() : describe(after.all().front()));
}

// While a queue runs its sets stay those it started with, so the check agrees
// with what edit() will take: a set dropped from the file meanwhile is still
// there for it.
TEST_F(NamedPeakCenterSessionTest, WhileAQueueRunsTheCheckIsTheRunningQueues) {
  write_peak_centers("\n[tuned]\nisotope = \"Ar40\"\ndetector = \"H1\"\nwindow = 0.07\nstep = 0.002\n"
                     "integration_s = 1.0\nn_tries = 1\nupdate_table = false\n");
  ASSERT_TRUE(session_->start(queue_));
  write_peak_centers("", true);
  EXPECT_TRUE(session_->check_queue(queue_).ok());
  session_->abort();
  ASSERT_TRUE(session_->wait().has_value());
  EXPECT_FALSE(session_->check_queue(queue_).ok());  // ended: the file as it is now
}

// A file that does not parse when the check is made is an error naming the
// file, with the text start() refuses with.
TEST_F(NamedPeakCenterSessionTest, TheCheckNamesABadFile) {
  write_peak_centers("\n[x]\nwindow = -1\n");
  const auto started = session_->start(queue_);
  ASSERT_FALSE(started);
  const auto c = session_->check_queue(queue_);
  ASSERT_FALSE(c.ok());
  bool named = false;
  for (const auto& d : c.all()) {
    if (d.severity == Severity::Error && d.field == "lab" && d.message == started.error().what) named = true;
  }
  EXPECT_TRUE(named) << started.error().what;
}

// A lab loaded while peak_center.toml was bad carries its error in
// Lab::problems; the file read at the start decides, so a fixed file starts
// the queue without a new load.
class BadPeakCenterAtLoadSessionTest : public NamedPeakCenterSessionTest {
 protected:
  void prepare_lab() override {
    NamedPeakCenterSessionTest::prepare_lab();
    write_peak_centers("\n[tuned]\nwindow = -1\n");
  }
  bool lab_loads_clean() override { return false; }
};

TEST_F(BadPeakCenterAtLoadSessionTest, AFixedFileStartsWithoutANewLoad) {
  ASSERT_FALSE(lab_.problems.empty());
  EXPECT_FALSE(session_->start(queue_));
  EXPECT_FALSE(session_->check_queue(queue_).ok());
  write_peak_centers("[tuned]\nisotope = \"Ar40\"\ndetector = \"H1\"\nwindow = 0.07\nstep = 0.002\n"
                     "integration_s = 1.0\nn_tries = 1\nupdate_table = false\n", true);
  // The load-time error is stale once the file is read again.
  EXPECT_TRUE(session_->check_queue(queue_).ok());
  std::mutex m;
  std::vector<double> windows;
  auto sub = line_->bus().subscribe<jobs::PeakCenterDone>([&](const jobs::PeakCenterDone& d) {
    std::lock_guard lock(m);
    windows.push_back(d.result.config.window);
  });
  const auto started = session_->start(queue_);
  ASSERT_TRUE(started) << started.error().what;
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
  std::lock_guard lock(m);
  ASSERT_FALSE(windows.empty());
  for (const double w : windows) EXPECT_DOUBLE_EQ(w, 0.07);
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
  expect_aborted_before_any_run();
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
    if (line.starts_with("Stage.MoveTo ")) {
      EXPECT_EQ(line.find("Stage.MoveTo -"), std::string::npos) << line;
    }
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
  {
    // The notifier's commands are the outside world's: not waited for in the clock.
    const Clock::Detached detached(clock_);
    session_->notifier().wait_idle();
  }
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

// A request made straight after start() comes before the queue's thread has
// got as far as the executor: it ends the queue all the same. The test's
// thread does not wait in the clock in between, so no run can have started.
TEST_F(LabSessionTest, AnAbortStraightAfterStartEndsTheQueueBeforeAnyRun) {
  ASSERT_TRUE(session_->start(queue_));
  session_->abort();
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Aborted) << result->reason;
  EXPECT_TRUE(result->runs.empty());
  EXPECT_EQ(started_.load(), 0);
  EXPECT_FALSE(fs::exists(dir_ / "data" / "records"));
}

TEST_F(LabSessionTest, ACancelStraightAfterStartEndsTheQueueBeforeAnyRun) {
  ASSERT_TRUE(session_->start(queue_));
  session_->cancel();
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Cancelled) << result->reason;
  EXPECT_TRUE(result->runs.empty());
  EXPECT_EQ(started_.load(), 0);
}

TEST_F(LabSessionTest, AStopStraightAfterStartEndsTheQueueBeforeAnyRun) {
  ASSERT_TRUE(session_->start(queue_));
  session_->stop();
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Stopped) << result->reason;
  EXPECT_TRUE(result->runs.empty());
  EXPECT_EQ(started_.load(), 0);
}

TEST_F(LabSessionTest, DestroyingASessionStraightAfterStartAbortsItsQueue) {
  ASSERT_TRUE(session_->start(queue_));
  session_.reset();
  ASSERT_EQ(ended().size(), 1u);
  EXPECT_EQ(ended().front().result.end, executor::QueueEnd::Aborted) << ended().front().result.reason;
  EXPECT_TRUE(ended().front().result.runs.empty());
  EXPECT_EQ(started_.load(), 0);
}

// A cancel from a thread the clock knows nothing of (the UI's, a signal's)
// reaches a queue that is asleep in simulated time. The test's thread keeps
// time where it is meanwhile, so the cancel lands in the first run.
TEST_F(LabSessionTest, ACancelFromOutsideTheClockEndsTheQueue) {
  ASSERT_TRUE(session_->start(queue_));
  ASSERT_TRUE(eventually([&] { return started_ > 0; }));
  const TimePoint at = clock_.now();
  std::thread outsider([&] { session_->cancel(); });
  outsider.join();  // not a participant: nothing for the clock to wait for
  EXPECT_EQ(clock_.now(), at);
  const auto result = session_->wait();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->end, executor::QueueEnd::Cancelled) << result->reason;
  // The run that had started is the only one, and it did not finish.
  ASSERT_EQ(result->runs.size(), 1u);
  EXPECT_EQ(result->runs.front().state, run::RunState::Cancelled);
  EXPECT_FALSE(session_->running());
}

// A QueueEnded subscriber may call back into the session while the owner
// starts the next queue (which joins the previous queue's thread).
TEST_F(LabSessionTest, AQueueEndedSubscriberMayCallBackIn) {
  std::atomic<int> called{0};
  subs_.push_back(line_->bus().subscribe<QueueEnded>([this, &called](const QueueEnded&) {
    clock_.sleep_for(200ms);  // still publishing when the next start() comes
    (void)session_->running();
    (void)session_->state();
    ++called;
  }));
  queue_.runs.resize(1);
  ASSERT_TRUE(session_->start(queue_));
  // Seen within 100 ms of the queue's end: the subscriber has 200 ms to go.
  ASSERT_TRUE(eventually([&] { return !session_->running(); }));
  EXPECT_EQ(called.load(), 0);
  ASSERT_TRUE(session_->start(queue_));  // joins the first thread mid-publish
  EXPECT_EQ(called.load(), 1);
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
