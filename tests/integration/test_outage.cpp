// An outage at any instant of a queue (executor hardening spec, sections 5
// and 6): the example sim lab on a VirtualClock runs its queue once without
// a fault, then once for each link the runs need and each instant in a list,
// with that link down for a while from that instant.
//
// A short outage is ridden out: the queue ends as it did, every run as it
// did, and every record has the series and the counts it had. A reading
// that waited for a link is later, so its time and value are not compared
// (spec 5, "only notes and counters differ", read with what a clock that
// moves allows). A long one is held for, and the rules of spec 5 hold.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/experiment/executor/links.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/trace_recorder.hpp"
#include "temp_path.hpp"
#include "virtual_time.hpp"

namespace pychron::experiment::lab {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

const TimePoint kStart = TimePoint{} + std::chrono::hours(1);

// What a queue came to, to compare.
struct Outcome {
  executor::QueueEnd end = executor::QueueEnd::Completed;
  std::string reason;
  std::vector<std::tuple<std::string, run::RunState>> runs;                // identifier, state, in order
  std::map<std::string, std::tuple<record::RecordStatus, std::map<std::string, std::size_t>>> records;  // by run id
  // For the rules.
  bool held = false;
  std::map<std::size_t, std::set<std::string>> extracted;  // row -> the runs of it that began extraction
  std::set<std::string> measured;                          // runs that reached Measuring
  std::vector<std::pair<std::string, TimePoint>> phases;   // every state change, with its time
  TimePoint began{}, ended{};
};

// One link down for `length` from `at` after the queue starts.
struct Fault {
  std::string transport;
  pychron::Duration at{};
  pychron::Duration length{};
};

// The example lab's line and spectrometer, simulated, on a clock of their own.
class World {
 public:
  World() : dir_(pychron::testing::unique_temp_path("pychron-outage-")), clock_(clock_options()) {
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_, fs::copy_options::recursive);
    fs::remove_all(dir_ / "data");
    systems::ExtractionLine::Options options;
    options.clock = &clock_;
    options.force_sim = true;
    options.state_file = dir_ / "line.state.toml";
    auto line = systems::ExtractionLine::load(dir_ / "extraction_line.toml", dir_ / "canvas.toml", options);
    EXPECT_TRUE(line) << line.error().what;
    line_ = std::move(*line);
    EXPECT_TRUE(line_->start());
    auto spec = spectrometer::load_spectrometer_for_app(
        dir_ / "spectrometer.sim-integrated.toml",
        spectrometer::SpectrometerContext{clock_, line_->scheduler(), line_->bus()},
        spectrometer::SpectrometerBringup{.sim_beam_from_table = true, .line_sim = line_->sim()});
    EXPECT_TRUE(spec) << spec.error().what;
    spec_ = std::move(*spec);
    scan_ = std::make_unique<spectrometer::ScanService>(*spec_, line_->bus(), clock_);
    lab_ = load_lab({dir_, dir_ / "extraction_line.toml", dir_ / "spectrometer.sim-integrated.toml"});
    EXPECT_TRUE(lab_.problems.empty()) << (lab_.problems.empty() ? "" : lab_.problems.front());
    auto q = load_queue_file((dir_ / "experiment.toml").string(), lab_.ids);
    EXPECT_TRUE(q) << q.error().what;
    queue_ = *q;
    if (!scripting::make_script_host()->available()) {
      for (auto& r : queue_.runs) {
        r.extraction.script.clear();
        r.post_measurement.reset();
      }
    }
    session_ = std::make_unique<LabSession>(lab_, SessionHardware{*line_, spec_.get(), scan_.get(), {}},
                                            SessionOptions{dir_ / "data", {}, {}});
    auto& bus = line_->bus();
    subs_.push_back(bus.subscribe<executor::ExecutorHolding>([this](const executor::ExecutorHolding&) {
      std::lock_guard lock(mutex_);
      out_.held = true;
    }));
    subs_.push_back(bus.subscribe<executor::RunStarted>([this](const executor::RunStarted& e) {
      std::lock_guard lock(mutex_);
      row_of_[e.run_id] = e.row;
    }));
    subs_.push_back(bus.subscribe<run::RunStateChanged>([this](const run::RunStateChanged& e) {
      std::lock_guard lock(mutex_);
      out_.phases.emplace_back(e.run_id + ":" + std::string(run::to_string(e.to)), e.ts);
      if (e.to == run::RunState::Extracting) out_.extracted[row_of_[e.run_id]].insert(e.run_id);
      if (e.to == run::RunState::Measuring) out_.measured.insert(e.run_id);
    }));
  }

  ~World() {
    subs_.clear();
    session_.reset();
    scan_.reset();
    if (line_) line_->stop();
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
    line_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  // The names of the transports a measured run needs.
  std::vector<std::string> required() const {
    RunSpec run;
    run.measurement.plan = "x";
    std::vector<std::string> out;
    for (Link* l : executor::required_links(run, line_->links(), spec_->links()).required)
      out.push_back(l->transport().name());
    return out;
  }

  Outcome run(std::optional<Fault> fault = std::nullopt) {
    pychron::testing::Crew crew(clock_);
    SimTransport* sim = fault ? sim_transport(fault->transport) : nullptr;
    if (fault) {
      EXPECT_NE(sim, nullptr) << fault->transport;
    }
    // Time 0 is the queue's start, told by the executor's own thread (this
    // one takes no part in the clock, which runs on while it works); the
    // fault is timed from there.
    auto started = line_->bus().subscribe<executor::ExecutorStateChanged>([&](const executor::ExecutorStateChanged& e) {
      if (e.to == executor::ExecutorState::Idle) {
        std::lock_guard lock(mutex_);
        out_.ended = clock_.now();  // the queue's end, on the executor's thread
        return;
      }
      if (e.to != executor::ExecutorState::Preparing) return;
      {
        std::lock_guard lock(mutex_);
        out_.began = clock_.now();
      }
      if (sim != nullptr)
        crew.start("fault", [this, sim, f = *fault] {
          clock_.sleep_for(f.at);
          sim->down_for(f.length);
          if (Transport* t = transport(f.transport)) t->close();  // as a dropped link is
        });
    });
    EXPECT_TRUE(session_->start(queue_));
    const auto result = session_->wait();
    crew.join();
    std::lock_guard lock(mutex_);
    if (result) {
      out_.end = result->end;
      out_.reason = result->reason;
      for (const auto& r : result->runs) out_.runs.emplace_back(r.identifier, r.state);
    }
    std::error_code ec;  // no records at all: a queue that ended before any was saved
    for (const auto& e : fs::recursive_directory_iterator(dir_ / "data" / "records", ec)) {
      const auto name = e.path().filename().string();
      if (e.path().extension() != ".json" || name.ends_with(".extraction.json")) continue;
      std::ifstream in(e.path());
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      auto rec = record::from_json(text);
      if (!rec) continue;
      std::map<std::string, std::size_t> series;
      for (const auto& s : rec->data.series) series[s.iso + "/" + s.det + "/" + s.kind] = s.trace.t.size();
      out_.records[rec->identity.uuid] = {rec->provenance.status, std::move(series)};
    }
    return out_;
  }

 private:
  static VirtualClock::Options clock_options() {
    VirtualClock::Options o;
    o.start = kStart;
    return o;
  }
  Transport* transport(const std::string& name) const {
    if (Transport* t = line_->transport(name)) return t;
    for (Link* l : spec_->links())
      if (l->transport().name() == name) return &l->transport();
    return nullptr;
  }
  SimTransport* sim_transport(const std::string& name) const {
    Transport* t = transport(name);
    if (auto* recorder = dynamic_cast<TraceRecorder*>(t)) t = &recorder->inner();
    return dynamic_cast<SimTransport*>(t);
  }

  fs::path dir_;
  VirtualClock clock_;
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<spectrometer::Spectrometer> spec_;
  std::unique_ptr<spectrometer::ScanService> scan_;
  Lab lab_;
  QueueSpec queue_;
  std::unique_ptr<LabSession> session_;
  std::mutex mutex_;
  Outcome out_;
  std::map<std::string, std::size_t> row_of_;
  std::vector<SignalBus::Subscription> subs_;
};

// The instants a fault starts at, after the queue starts: every phase
// boundary of every run of the fault-free queue, and `extra` from a seed.
std::vector<pychron::Duration> instants(const Outcome& clean, std::size_t extra, std::uint32_t seed) {
  std::vector<pychron::Duration> out;
  out.reserve(clean.phases.size() + extra);
  for (const auto& [what, ts] : clean.phases) out.push_back(ts - clean.began);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> at(0.0, std::chrono::duration<double>(clean.ended - clean.began).count());
  for (std::size_t i = 0; i < extra; ++i)
    out.push_back(std::chrono::duration_cast<pychron::Duration>(std::chrono::duration<double>(at(rng))));
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string describe(const Fault& f) {
  return f.transport + " down " + std::to_string(std::chrono::duration<double>(f.length).count()) + " s from " +
         std::to_string(std::chrono::duration<double>(f.at).count()) + " s";
}

// What a short outage must not change.
void expect_the_same(const Outcome& clean, const Outcome& faulted, const Fault& f) {
  EXPECT_EQ(faulted.end, clean.end) << describe(f) << ": " << faulted.reason;
  EXPECT_EQ(faulted.runs, clean.runs) << describe(f);
  ASSERT_EQ(faulted.records.size(), clean.records.size()) << describe(f);
  // Run ids differ between queues: compare in order of the runs.
  std::vector<std::tuple<record::RecordStatus, std::map<std::string, std::size_t>>> a, b;
  for (const auto& [id, r] : clean.records) a.push_back(r);
  for (const auto& [id, r] : faulted.records) b.push_back(r);
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  EXPECT_EQ(b, a) << describe(f);
}

// The rules of spec 5 for any outage.
void expect_the_rules(const Outcome& o, const Fault& f) {
  for (const auto& [row, runs] : o.extracted)
    EXPECT_LE(runs.size(), 1u) << describe(f) << ": row " << row << " started again past its point of no return";
  for (const auto& id : o.measured) EXPECT_TRUE(o.records.contains(id)) << describe(f) << ": no record of " << id;
  if (o.end != executor::QueueEnd::Completed) {
    EXPECT_TRUE(o.held) << describe(f) << ": ended without holding: " << o.reason;
  }
}

class Outage : public pychron::testing::VirtualTimeTest {
 protected:
  Outage() : VirtualTimeTest(std::chrono::seconds(600)) {}
};

TEST_F(Outage, AFaultFreeQueueCompletes) {
  World w;
  const auto o = w.run();
  EXPECT_EQ(o.end, executor::QueueEnd::Completed) << o.reason;
  EXPECT_FALSE(o.held);
  EXPECT_FALSE(o.records.empty());
}

// One case a required link (by index): its fault-free queue, then a fault
// of that link at each instant. Indices past the lab's links are skipped.
class LinkOutage : public pychron::testing::VirtualTimeTest, public ::testing::WithParamInterface<int> {
 protected:
  LinkOutage() : VirtualTimeTest(std::chrono::seconds(600)) {}

  // The fault-free queue and the link of this case; false: no such link.
  bool prepare() {
    World w;
    clean_ = w.run();
    const auto links = w.required();
    EXPECT_EQ(clean_.end, executor::QueueEnd::Completed) << clean_.reason;
    if (std::cmp_greater_equal(GetParam(), links.size())) return false;
    link_ = links[static_cast<std::size_t>(GetParam())];
    return true;
  }

  Outcome clean_;
  std::string link_;
};

constexpr std::size_t kSeededInstants = 10;
constexpr std::uint32_t kSeed = 20261015;

// 5 s down at any instant: ridden out, nothing measured changes.
TEST_P(LinkOutage, EveryCut) {
  if (!prepare()) GTEST_SKIP() << "the lab has fewer links";
  for (const auto at : instants(clean_, kSeededInstants, kSeed)) {
    const Fault f{link_, at, 5s};
    World w;
    const auto o = w.run(f);
    expect_the_same(clean_, o, f);
    expect_the_rules(o, f);
  }
}

// 10 min down at any instant: held for, and the rules hold.
TEST_P(LinkOutage, TenMinutes) {
  if (!prepare()) GTEST_SKIP() << "the lab has fewer links";
  int held = 0;
  for (const auto at : instants(clean_, kSeededInstants, kSeed)) {
    const Fault f{link_, at, 600s};
    World w;
    const auto o = w.run(f);
    expect_the_rules(o, f);
    held += o.held;
  }
  EXPECT_GT(held, 0) << link_ << ": no outage was held for: the faults reached nothing";
}

INSTANTIATE_TEST_SUITE_P(Links, LinkOutage, ::testing::Range(0, 8));

TEST_F(Outage, TheSameSeedGivesTheSameResult) {
  Outcome clean;
  std::string link;
  {
    World w;
    clean = w.run();
    link = w.required().front();
  }
  const auto at = instants(clean, kSeededInstants, kSeed);
  const Fault f{link, at[at.size() / 3], 600s};
  Outcome a, b;
  {
    World w;
    a = w.run(f);
  }
  {
    World w;
    b = w.run(f);
  }
  EXPECT_EQ(a.end, b.end);
  EXPECT_EQ(a.reason, b.reason);
  EXPECT_EQ(a.runs, b.runs);
  std::vector<std::tuple<record::RecordStatus, std::map<std::string, std::size_t>>> ra, rb;
  for (const auto& [id, r] : a.records) ra.push_back(r);
  for (const auto& [id, r] : b.records) rb.push_back(r);
  std::sort(ra.begin(), ra.end());
  std::sort(rb.begin(), rb.end());
  EXPECT_EQ(ra, rb);
  // Not the queue's length: the fault lands at an instant at which the
  // line's own scan may use the same transport, and which of the two goes
  // first at one instant is the threads' order, not the clock's. That moves
  // when a reconnect sees the link back by a second or a backoff, never what
  // the queue comes to. (A fault-free queue is exact: AFaultFreeQueueIsExact.)
}

TEST_F(Outage, AFaultFreeQueueIsExact) {
  Outcome a, b;
  {
    World w;
    a = w.run();
  }
  {
    World w;
    b = w.run();
  }
  EXPECT_EQ(a.ended - a.began, b.ended - b.began);
  ASSERT_EQ(a.phases.size(), b.phases.size());
  for (std::size_t i = 0; i < a.phases.size(); ++i) EXPECT_EQ(a.phases[i].second - a.began, b.phases[i].second - b.began) << i;
}

}  // namespace
}  // namespace pychron::experiment::lab
