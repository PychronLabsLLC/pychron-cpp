// MeasurementEngine against fake ports on a ManualClock: block order, hop
// runner semantics, equilibration timing, time zero, conditionals,
// truncate/terminate/cancel/abort, safety on exit, hooks, and results.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/measurement/results.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::measurement;
using namespace std::chrono_literals;
using collect::SeriesKey;
using collect::SeriesKind;

namespace {

double secs(pychron::Duration d) { return std::chrono::duration<double>(d).count(); }

// Intensity = abundance of the isotope on the detector at the magnet target,
// growing linearly with time since t0: v = a * (1 + 0.01 t).
class FakeSpectrometer final : public ISpectrometerPort {
 public:
  explicit FakeSpectrometer(ManualClock& clock) : clock_(clock) {}

  Result<void> position(const plan::HopTarget& target) override {
    log.push_back("position " + (target.mass ? std::to_string(*target.mass) : target.isotope) + "@" + target.detector);
    if (fail_position) return fail(ErrorKind::Io, "magnet fault");
    at_ = target;
    return {};
  }
  Result<void> protect(const std::string& det, bool on) override {
    log.push_back(std::string(on ? "protect " : "unprotect ") + det);
    if (on) protected_.insert(det); else protected_.erase(det);
    return {};
  }
  Result<void> start_acquisition(pychron::Duration integration) override {
    log.push_back("start " + std::to_string(secs(integration)));
    integration_ = integration;
    acquiring = true;
    return {};
  }
  Result<std::optional<spectrometer::Reading>> next_reading(pychron::Duration) override {
    if (!acquiring) return fail(ErrorKind::Config, "not acquiring");
    ++readings;
    if (fail_at && readings == *fail_at) return fail(ErrorKind::Timeout, "acquirer stalled");
    clock_.advance(integration_);
    spectrometer::Reading r;
    r.ts = clock_.now();
    r.integration = integration_;
    const double t = secs(clock_.now() - TimePoint{});
    for (const auto& det : detectors) {
      double v = 0.0;
      if (at_ && !at_->mass) {
        // The isotope sitting on `det` given where the reference was put.
        auto it = layout.find(at_->isotope + "@" + at_->detector);
        if (it != layout.end()) {
          auto iso = it->second.find(det);
          if (iso != it->second.end()) v = abundance.at(iso->second) * (1 + 0.01 * t);
        }
      }
      r.values[det] = spectrometer::Value{v, std::nullopt, std::nullopt, false};
    }
    if (on_reading) on_reading(readings);
    return std::optional<spectrometer::Reading>(std::move(r));
  }
  void stop_acquisition() override {
    log.push_back("stop");
    acquiring = false;
  }

  std::vector<std::string> log;
  std::vector<std::string> detectors{"H1", "AX", "L1", "CDD"};
  // "<iso>@<det>" magnet setting -> detector -> isotope it sees.
  std::map<std::string, std::map<std::string, std::string>> layout{
      {"Ar40@H1", {{"H1", "Ar40"}, {"AX", "Ar39"}, {"CDD", "Ar36"}}},
      {"Ar39@CDD", {{"CDD", "Ar39"}, {"H1", "Ar41"}}},
  };
  std::map<std::string, double> abundance{{"Ar40", 1000}, {"Ar39", 100}, {"Ar36", 3}, {"Ar41", 0}};
  std::function<void(int)> on_reading;
  std::optional<int> fail_at;
  bool fail_position = false;
  bool acquiring = false;
  int readings = 0;
  std::set<std::string> protected_;

 private:
  ManualClock& clock_;
  std::optional<plan::HopTarget> at_;
  pychron::Duration integration_ = 1s;
};

class FakeValves final : public IValvePort {
 public:
  explicit FakeValves(ManualClock& clock) : clock_(clock) {}
  Result<void> open(const std::string& v) override {
    log.push_back("open " + v + " @" + std::to_string(secs(clock_.now() - t0)));
    state[v] = true;
    return {};
  }
  Result<void> close(const std::string& v) override {
    log.push_back("close " + v + " @" + std::to_string(secs(clock_.now() - t0)));
    state[v] = false;
    return {};
  }
  std::vector<std::string> log;
  std::map<std::string, bool> state;
  TimePoint t0{};

 private:
  ManualClock& clock_;
};

class FakePeakCenter final : public IPeakCenterPort {
 public:
  Result<PeakCenterReport> peak_center(const PeakCenterRequest& req, scripting::CancelToken&) override {
    requests.push_back(req);
    return PeakCenterReport{req, ok, ok ? std::optional<double>(8.0) : std::nullopt, ok ? "" : "no peak"};
  }
  std::vector<PeakCenterRequest> requests;
  bool ok = true;
};

class FakeHook final : public IMeasurementHook {
 public:
  Result<void> call(std::string_view entry, scripting::IMeasurementApi& api, scripting::CancelToken&,
                    const scripting::ValueMap& args) override {
    entries.emplace_back(entry);
    if (auto it = args.find("result"); it != args.end()) whiff_result = std::get<std::string>(it->second);
    if (auto it = actions.find(std::string(entry)); it != actions.end()) return it->second(api);
    return {};
  }
  std::vector<std::string> entries;
  std::string whiff_result;
  std::map<std::string, std::function<Result<void>(scripting::IMeasurementApi&)>> actions;
};

plan::Hop hop(std::map<std::string, std::string> positions, int counts, double settle = 2) {
  plan::Hop h;
  h.positions = std::move(positions);
  h.counts = counts;
  h.settle_s = settle;
  return h;
}

plan::MeasurementPlan multicollect(int counts = 10, int cycles = 1) {
  plan::MeasurementPlan p;
  p.info.name = "mc";
  p.info.instrument_family = "fake";
  p.detectors.reference = "H1";
  p.equilibration = {"B", "C", 10, 2, true};
  p.sniff = {true, 10, 1};
  p.baseline = {false, false, 5, 34.2, "H1", 3, 1};
  p.main.cycles = cycles;
  p.main.integration_s = 1;
  p.main.hops.push_back(hop({{"Ar40", "H1"}, {"Ar39", "AX"}, {"Ar36", "CDD"}}, counts));
  return p;
}

Conditional conditional(ConditionalKind kind, std::string name, std::string check, int start = 0,
                        ActionSpec action = {}) {
  Conditional c;
  c.kind = kind;
  c.name = std::move(name);
  c.check = std::move(check);
  auto e = parse_expression(c.check);
  EXPECT_TRUE(e) << c.check;
  c.expr = std::shared_ptr<const Expr>(std::move(*e));
  c.start = start;
  c.action = action;
  if (kind == ConditionalKind::Truncation && action.type == ActionSpec::Type::None)
    c.action.type = ActionSpec::Type::Truncate;
  return c;
}

class EngineTest : public ::testing::Test {
 protected:
  EngineTest() { valves_.t0 = t0_; }

  MeasurementResult run(plan::MeasurementPlan p, ConditionalSet conditionals = {}, EngineOptions options = {}) {
    MeasurementInputs in{std::move(p), std::move(conditionals), {}, "run-1"};
    if (!options.sleep) options.sleep = [this](pychron::Duration d) { clock_.advance(d); };
    engine_ = std::make_unique<MeasurementEngine>(context(), std::move(in), std::move(options));
    return engine_->run(token_);
  }

  EngineContext context() {
    return EngineContext{spec_, clock_, &valves_, &peak_, &hook_, &bus_, nullptr};
  }

  int count(const MeasurementResult& r, const SeriesKey& key) {
    auto it = r.data.series.find(key);
    return it == r.data.series.end() ? 0 : static_cast<int>(it->second.v.size());
  }

  TimePoint t0_ = TimePoint{} + 1000s;
  ManualClock clock_{t0_};
  SignalBus bus_;
  FakeSpectrometer spec_{clock_};
  FakeValves valves_{clock_};
  FakePeakCenter peak_;
  FakeHook hook_;
  scripting::CancelToken token_;
  std::unique_ptr<MeasurementEngine> engine_;
};

TEST_F(EngineTest, RunsTheBlockSequenceInOrder) {
  auto p = multicollect();
  p.peak_center = {true, true, "", "Ar40", "default"};
  p.baseline.before = p.baseline.after = true;
  std::vector<std::string> events;
  auto s1 = bus_.subscribe<BlockStarted>([&](const BlockStarted& e) { events.push_back("+" + std::string(to_string(e.block))); });
  auto s2 = bus_.subscribe<BlockFinished>([&](const BlockFinished& e) {
    events.push_back("-" + std::string(to_string(e.block)) + (e.ok ? "" : "!"));
  });
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(r.blocks, (std::vector<Block>{Block::PeakCenterBefore, Block::BaselineBefore, Block::PositionFirstHop,
                                          Block::Equilibrate, Block::Main, Block::BaselineAfter,
                                          Block::PeakCenterAfter}));
  EXPECT_EQ(events.front(), "+peak_center.before");
  EXPECT_EQ(events.back(), "-peak_center.after");
  ASSERT_EQ(peak_.requests.size(), 2u);
  EXPECT_EQ(peak_.requests[0].detector, "H1");  // empty -> reference
  EXPECT_EQ(peak_.requests[0].isotope, "Ar40");
  EXPECT_EQ(r.peak_centers.size(), 2u);
  // Baselines collect per detector (no isotope) on every detector of the hops.
  EXPECT_EQ(count(r, {"", "H1", SeriesKind::Baseline}), 10);
  EXPECT_EQ(count(r, {"", "CDD", SeriesKind::Baseline}), 10);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 10);
  EXPECT_EQ(r.data.counts.at("main"), 10);
  EXPECT_EQ(r.data.counts.at("baseline.before"), 5);
  EXPECT_EQ(r.data.counts.at("baseline.after"), 5);
}

TEST_F(EngineTest, EquilibrationTimingAndTimeZeroOnInletClose) {
  int overlap = 0, closed_cb = 0;
  auto sub = bus_.subscribe<OverlapReady>([&](const OverlapReady& e) {
    EXPECT_EQ(e.run_id, "run-1");
    ++overlap;
  });
  EngineOptions options;
  options.on_inlet_closed = [&] { ++closed_cb; };
  auto r = run(multicollect(), {}, options);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  // Outlet first; inlet opens after inlet_delay (2 s, at reading
  // granularity: sniff readings are 1 s) and closes time_s (10 s) later.
  ASSERT_EQ(valves_.log.size(), 3u);
  EXPECT_EQ(valves_.log[0].substr(0, 7), "close C");
  EXPECT_EQ(valves_.log[1].substr(0, 6), "open B");
  EXPECT_EQ(valves_.log[2].substr(0, 7), "close B");
  const auto& tm = r.data.timing;
  ASSERT_TRUE(tm.inlet_open && tm.inlet_close && tm.time_zero);
  EXPECT_NEAR(*tm.inlet_close - *tm.inlet_open, 10.0, 1e-9);
  EXPECT_DOUBLE_EQ(*tm.time_zero, *tm.inlet_close);
  EXPECT_EQ(overlap, 1);
  EXPECT_EQ(closed_cb, 1);
  // Sniff stops at its counts; equilibration still runs its full time.
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Sniff}), 10);
  EXPECT_FALSE(valves_.state["B"]);
}

TEST_F(EngineTest, EquilibrationWithoutSniffWaitsOnTheClock) {
  auto p = multicollect(3);
  p.sniff.enabled = false;
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed);
  EXPECT_DOUBLE_EQ(*r.data.timing.inlet_open, 2.0 + 2.0);  // first-hop settle, then inlet_delay
  EXPECT_DOUBLE_EQ(*r.data.timing.inlet_close, 14.0);
}

TEST_F(EngineTest, TimeZeroOnFirstCountAndOffset) {
  auto p = multicollect(3);
  p.sniff.enabled = false;
  p.main.time_zero = {plan::TimeZeroKind::OnFirstCount, 0};
  auto r = run(p);
  const auto& s = r.data.series.at({"Ar40", "H1", SeriesKind::Signal});
  EXPECT_DOUBLE_EQ(*r.data.timing.time_zero, s.t.front());

  p.main.time_zero = {plan::TimeZeroKind::Offset, 4};
  r = run(p);
  EXPECT_DOUBLE_EQ(*r.data.timing.time_zero, *r.data.timing.inlet_open + 4);
}

TEST_F(EngineTest, NoCloseInletKeepsItOpenAndTimeZeroAtEndOfEquilibration) {
  auto p = multicollect(3);
  p.equilibration.close_inlet = false;
  int overlap = 0;
  auto sub = bus_.subscribe<OverlapReady>([&](const OverlapReady&) { ++overlap; });
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed);
  EXPECT_TRUE(valves_.state["B"]);
  EXPECT_EQ(overlap, 0);
  EXPECT_FALSE(r.data.timing.inlet_close);
  EXPECT_NEAR(*r.data.timing.time_zero - *r.data.timing.inlet_open, 10.0, 1e-9);
}

TEST_F(EngineTest, OneHopTimesNCyclesEqualsOneHopWithNTimesCounts) {
  auto a = multicollect(5, 4);
  auto ra = run(a);
  auto log_a = spec_.log;
  const auto end_a = clock_.now();

  clock_.set(t0_);
  spec_.log.clear();
  auto rb = run(multicollect(20, 1));
  ASSERT_EQ(ra.outcome, MeasurementOutcome::Completed);
  ASSERT_EQ(rb.outcome, MeasurementOutcome::Completed);
  EXPECT_EQ(ra.data.series.size(), rb.data.series.size());
  for (const auto& [key, s] : ra.data.series) {
    if (key.kind == SeriesKind::Signal) {
      EXPECT_EQ(s.v.size(), 20u) << to_string(key);
      EXPECT_EQ(s.t, rb.data.series.at(key).t) << to_string(key);
      EXPECT_EQ(s.v, rb.data.series.at(key).v) << to_string(key);
    }
  }
  EXPECT_EQ(end_a, clock_.now());
  // One move; later cycles skip move and settle. Acquisition restarts per cycle.
  EXPECT_EQ(std::count_if(log_a.begin(), log_a.end(), [](auto& l) { return l.starts_with("position"); }), 1);
  EXPECT_EQ(std::count_if(spec_.log.begin(), spec_.log.end(), [](auto& l) { return l.starts_with("position"); }), 1);
}

TEST_F(EngineTest, PeakHopOrderingProtectionAndBaselineHops) {
  auto p = multicollect();
  p.sniff.enabled = false;
  p.main.cycles = 2;
  p.main.hops.clear();
  auto h0 = hop({{"Ar40", "H1"}, {"Ar36", "CDD"}}, 3);
  h0.protect = {"CDD"};
  auto h1 = hop({{"Ar39", "CDD"}}, 2);
  h1.position = plan::HopPosition{"Ar39", "CDD"};
  auto h2 = hop({{"Ar36", "CDD"}}, 1);
  h2.baseline = true;
  h2.mass = 34.2;
  h2.position = plan::HopPosition{"", "CDD"};
  p.main.hops = {h0, h1, h2};
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");

  std::vector<std::string> moves;
  for (const auto& l : spec_.log)
    if (l.starts_with("position") || l.starts_with("protect") || l.starts_with("unprotect")) moves.push_back(l);
  // First hop positioned (with protection) by PositionFirstHop; main's first
  // hop is already there. Every later hop changes target, so it moves.
  const std::vector<std::string> expected{
      "protect CDD", "position Ar40@H1", "unprotect CDD",  // position block
      "position Ar39@CDD", "position 34.200000@CDD",        // cycle 1
      "protect CDD", "position Ar40@H1", "unprotect CDD",  // cycle 2
      "position Ar39@CDD", "position 34.200000@CDD",
  };
  EXPECT_EQ(moves, expected);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 6);
  EXPECT_EQ(count(r, {"Ar36", "CDD", SeriesKind::Signal}), 6);
  EXPECT_EQ(count(r, {"Ar39", "CDD", SeriesKind::Signal}), 4);
  EXPECT_EQ(count(r, {"Ar36", "CDD", SeriesKind::Baseline}), 2);
  EXPECT_EQ(r.data.counts.at("main"), 12);
  EXPECT_TRUE(spec_.protected_.empty());
  // Ar39 on CDD reads Ar39's abundance, not Ar36's.
  EXPECT_NEAR(r.data.series.at({"Ar39", "CDD", SeriesKind::Signal}).v.front() /
                  r.data.series.at({"Ar36", "CDD", SeriesKind::Signal}).v.front(),
              100.0 / 3.0, 1.0);
}

TEST_F(EngineTest, TruncationScalesLaterCollections) {
  auto p = multicollect(40);
  p.sniff.enabled = false;
  p.baseline.after = true;
  p.baseline.counts = 30;
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Truncation, "big", "Ar40.cur > 0", 10));
  set.items.back().abbreviated_count_ratio = 0.3;
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Truncated);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 11);  // first check after reading start + 1
  EXPECT_DOUBLE_EQ(r.count_scale, 0.3);                         // the conditional's ratio (pychron)
  EXPECT_EQ(r.data.counts.at("baseline.after"), 9);  // ceil(30 * 0.3)
  ASSERT_EQ(r.data.trips.size(), 1u);
  EXPECT_EQ(r.data.trips[0].name, "big");
}

TEST_F(EngineTest, UserQuickTruncateScalesByAQuarter) {
  auto p = multicollect(40, 3);
  p.sniff.enabled = false;
  p.baseline.after = true;
  p.baseline.counts = 30;
  spec_.on_reading = [&](int n) {
    if (n == 5) engine_->truncate(true);
  };
  auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Truncated);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 5);  // requested while reading 5 was in flight
  EXPECT_DOUBLE_EQ(r.count_scale, 0.25);
  EXPECT_EQ(r.data.counts.at("baseline.after"), 8);  // ceil(30 * 0.25); later cycles skipped
}

TEST_F(EngineTest, TerminationKeepsDataAndSkipsLaterBlocks) {
  auto p = multicollect(40);
  p.sniff.enabled = false;
  p.baseline.after = true;
  p.peak_center.after = true;
  p.peak_center.isotope = "Ar40";
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Termination, "stop", "count(Ar40) >= 7"));
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Terminated);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 7);
  EXPECT_EQ(r.blocks.back(), Block::Main);
  EXPECT_TRUE(peak_.requests.empty());
  EXPECT_FALSE(valves_.state["B"]);
}

TEST_F(EngineTest, ActionsAndModifications) {
  auto p = multicollect(10);
  p.sniff.enabled = false;
  p.baseline.after = true;
  p.baseline.counts = 30;
  p.hook = "h.py";
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Modification, "skip", "Ar40.cur > 0", 2,
                                  ActionSpec{ActionSpec::Type::SkipAliquot}));
  // First trip wins: a resuming action that stays true would starve the
  // actions after it, so this one is true at reading 4 only.
  set.items.push_back(conditional(ConditionalKind::Action, "note", "count(Ar40) == 4", 3,
                                  ActionSpec{ActionSpec::Type::Notify}));
  set.items.back().resume = true;
  set.items.push_back(conditional(ConditionalKind::Action, "hook", "count(Ar40) == 5", 4,
                                  ActionSpec{ActionSpec::Type::RunHook, false, "on_big"}));
  set.items.back().resume = true;
  set.items.push_back(conditional(ConditionalKind::Action, "cut", "Ar40.cur > 0", 6,
                                  ActionSpec{ActionSpec::Type::Truncate}));
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Truncated);
  ASSERT_EQ(r.modifications.size(), 1u);
  EXPECT_EQ(r.modifications[0].action.type, ActionSpec::Type::SkipAliquot);
  EXPECT_EQ(r.modifications[0].name, "skip");
  EXPECT_EQ(r.notes, (std::vector<std::string>{"notify: note"}));
  EXPECT_EQ(hook_.entries, (std::vector<std::string>{"before_main", "on_big", "after_main"}));
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 7);
  EXPECT_DOUBLE_EQ(r.count_scale, 1.0);  // a user/action truncate keeps later counts
  EXPECT_EQ(r.data.counts.at("baseline.after"), 30);
  EXPECT_FALSE(r.cancel_queue);
  EXPECT_EQ(r.installed.size(), 4u);
}

TEST_F(EngineTest, NonResumingActionEndsMainWithoutTruncating) {
  auto p = multicollect(10, 3);
  p.sniff.enabled = false;
  p.baseline.after = true;
  p.baseline.counts = 30;
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Action, "note", "Ar40.cur > 0", 3,
                                  ActionSpec{ActionSpec::Type::Notify}));
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 4);  // later cycles skipped
  EXPECT_EQ(r.data.counts.at("baseline.after"), 30);
  EXPECT_EQ(r.notes, (std::vector<std::string>{"notify: note"}));
}

TEST_F(EngineTest, ModificationFlagsActInRun) {
  auto p = multicollect(20);
  p.sniff.enabled = false;
  p.baseline.after = true;
  p.baseline.counts = 20;
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Modification, "low", "Ar40.cur > 0", 4,
                                  ActionSpec{ActionSpec::Type::RunBlank}));
  set.items.back().truncate = true;
  set.items.back().abbreviated_count_ratio = 0.5;
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Truncated);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 5);
  EXPECT_EQ(r.data.counts.at("baseline.after"), 10);
  ASSERT_EQ(r.modifications.size(), 1u);
  EXPECT_TRUE(r.modifications[0].truncate);

  set.items.back().truncate = false;
  set.items.back().terminate = true;
  r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Terminated);
  EXPECT_EQ(r.blocks.back(), Block::Main);
}

TEST_F(EngineTest, AnalysisTypeFiltersConditionals) {
  auto p = multicollect(10);
  p.sniff.enabled = false;
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Termination, "blanks_only", "Ar40.cur > 0", 2));
  set.items.back().analysis_types = {"blank"};
  MeasurementInputs in{p, set, {}, "run-1"};
  in.analysis_type = "unknown";
  EngineOptions options;
  options.sleep = [this](pychron::Duration d) { clock_.advance(d); };
  MeasurementEngine e(context(), in, options);
  auto r = e.run(token_);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed);
  EXPECT_TRUE(r.installed.empty());
  in.analysis_type = "blank_unknown";
  MeasurementEngine e2(context(), in, options);
  EXPECT_EQ(e2.run(token_).outcome, MeasurementOutcome::Terminated);
}

TEST_F(EngineTest, TripsCarryProvenance) {
  auto p = multicollect(10);
  p.sniff.enabled = false;
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Termination, "t", "Ar40.cur > 0 and Ar39.cur > 0", 4));
  set.stamp(ConditionalLevel::Queue, "q.toml");
  std::vector<ConditionalTripped> events;
  auto sub = bus_.subscribe<ConditionalTripped>([&](const ConditionalTripped& e) { events.push_back(e); });
  auto r = run(p, set);
  ASSERT_EQ(r.data.trips.size(), 1u);
  const auto& t = r.data.trips[0];
  EXPECT_EQ(t.reading, 5);
  EXPECT_EQ(t.level, ConditionalLevel::Queue);
  EXPECT_EQ(t.id, set.items[0].id());
  ASSERT_EQ(t.context.size(), 2u);
  EXPECT_EQ(t.context[0].metric, "Ar40.cur");
  EXPECT_GT(t.context[0].value, 0);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].run_id, "run-1");
  ASSERT_EQ(r.installed.size(), 1u);
  EXPECT_EQ(r.installed[0].location, "q.toml");
}

TEST_F(EngineTest, CancelationAsksToCancelTheQueue) {
  auto p = multicollect(10);
  p.sniff.enabled = false;
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Cancelation, "bad", "Ar40.cur > 0", 1));
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Cancelled);
  EXPECT_TRUE(r.cancel_queue);
  token_.reset();
  auto user = multicollect(40);
  spec_.on_reading = [&](int n) {
    if (n == 20) token_.cancel();
  };
  r = run(user);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Cancelled);
  EXPECT_FALSE(r.cancel_queue);  // a user cancel is the executor's call
}

TEST_F(EngineTest, WhiffRunRemainderPumpAbort) {
  auto p = multicollect(5);
  p.hook = "h.py";
  // The fake's Ar40 is ~1000 * (1 + 0.01 t) with t ~ 1000 s, i.e. ~1.1e4.
  p.whiff = {true, 3, 1, {{"Ar40.cur > 1e9", "abort"}, {"Ar40.cur > 1e6", "pump"}}};
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed)
      << to_string(r.outcome) << " " << (r.error ? r.error->what : "") << " blocks=" << r.blocks.size()
      << " notes=" << testing::PrintToString(r.notes);
  EXPECT_EQ(r.whiff, WhiffCheck::Action::RunRemainder);  // nothing matched
  EXPECT_EQ(hook_.whiff_result, "run_remainder");
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Whiff}), 3);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Sniff}), 7);  // the rest of the 10 s equilibration
  // The whiff happens after the inlet opens.
  EXPECT_GT(r.data.series.at({"Ar40", "H1", SeriesKind::Whiff}).t.front(), *r.data.timing.inlet_open);

  p.whiff.checks = {{"Ar40.cur > 500", "pump"}};
  valves_.log.clear();
  r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Terminated);
  EXPECT_EQ(r.whiff, WhiffCheck::Action::Pump);
  EXPECT_FALSE(valves_.state["B"]);
  EXPECT_TRUE(valves_.state["C"]);  // outlet opened to pump the gas away
  EXPECT_EQ(r.blocks.back(), Block::Equilibrate);
  EXPECT_FALSE(r.data.series.contains({"Ar40", "H1", SeriesKind::Signal}));

  p.whiff.checks = {{"Ar40.cur > 500", "abort"}};
  r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Aborted);
  EXPECT_FALSE(valves_.state["B"]);
}

TEST_F(EngineTest, EquilibrationConditionalClosesTheInletEarly) {
  auto p = multicollect(3);
  p.sniff.counts = 30;
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Equilibration, "full", "count(Ar40) >= 3"));
  auto r = run(p, set);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed);
  // Inlet opened at 4 s (settle 2 + delay 2); sniff count 3 is reached at 5 s.
  EXPECT_DOUBLE_EQ(*r.data.timing.inlet_close, 5.0);
  EXPECT_LT(*r.data.timing.inlet_close - *r.data.timing.inlet_open, 10.0);
}

TEST_F(EngineTest, CancelMidMainLeavesTheLineSafe) {
  auto p = multicollect(40);
  p.equilibration.close_inlet = false;  // the inlet is still open during main
  p.main.hops[0].protect = {"CDD"};
  p.baseline.after = true;
  spec_.on_reading = [&](int n) {
    if (n == 20) token_.cancel();
  };
  auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Cancelled);
  EXPECT_FALSE(valves_.state["B"]);  // closed by cleanup
  EXPECT_FALSE(spec_.acquiring);
  EXPECT_TRUE(spec_.protected_.empty());
  EXPECT_EQ(r.blocks.back(), Block::Main);
  EXPECT_LT(count(r, {"Ar40", "H1", SeriesKind::Signal}), 40);
}

TEST_F(EngineTest, AbortDuringEquilibrationClosesTheInlet) {
  auto p = multicollect(10);
  spec_.on_reading = [&](int n) {
    if (n == 5) token_.abort();
  };
  auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Aborted);
  EXPECT_FALSE(valves_.state["B"]);
  EXPECT_FALSE(spec_.acquiring);
  EXPECT_EQ(r.blocks.back(), Block::Equilibrate);
}

TEST_F(EngineTest, CancelationConditionalCancels) {
  auto p = multicollect(10);
  p.sniff.enabled = false;
  ConditionalSet set;
  set.items.push_back(conditional(ConditionalKind::Cancelation, "bad", "Ar40 > 0", 2));
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Cancelled);
  EXPECT_EQ(count(r, {"Ar40", "H1", SeriesKind::Signal}), 3);
}

TEST_F(EngineTest, AcquisitionFailureFailsAndCleansUp) {
  auto p = multicollect(10);
  p.equilibration.close_inlet = false;
  spec_.fail_at = 15;  // inside main (10 sniff readings first)
  auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_EQ(r.error->kind, ErrorKind::Timeout);
  EXPECT_FALSE(spec_.acquiring);
  EXPECT_FALSE(valves_.state["B"]);
}

TEST_F(EngineTest, PositionFailureFails) {
  spec_.fail_position = true;
  auto r = run(multicollect());
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  EXPECT_EQ(r.blocks.back(), Block::PositionFirstHop);
  EXPECT_TRUE(valves_.log.empty());
}

TEST_F(EngineTest, FailedPeakCenterIsReportedAndForcesARepositioning) {
  auto p = multicollect(3);
  p.sniff.enabled = false;
  p.peak_center = {true, false, "AX", "Ar39", "default"};
  peak_.ok = false;
  std::vector<std::string> warnings;
  auto sub = bus_.subscribe<Log>([&](const Log& l) {
    if (l.level == LogLevel::Warn) warnings.push_back(l.message);
  });
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed);
  ASSERT_EQ(r.peak_centers.size(), 1u);
  EXPECT_FALSE(r.peak_centers[0].ok);
  EXPECT_EQ(peak_.requests[0].detector, "AX");
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("no peak"), std::string::npos);
}

TEST_F(EngineTest, ValidateNamesMissingServices) {
  auto p = multicollect();
  p.peak_center.after = true;
  p.hook = "h.py";
  EngineContext ctx{spec_, clock_};
  MeasurementEngine e(ctx, MeasurementInputs{p, {}, {}, "r"});
  auto v = e.validate();
  ASSERT_FALSE(v);
  EXPECT_NE(v.error().what.find("valve service"), std::string::npos);
  EXPECT_NE(v.error().what.find("peak-center"), std::string::npos);
  EXPECT_NE(v.error().what.find("hook 'h.py'"), std::string::npos);
  auto r = e.run(token_);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  EXPECT_TRUE(r.blocks.empty());
}

TEST_F(EngineTest, HookUsesTheMeasurementApi) {
  auto p = multicollect(4);
  p.sniff.enabled = false;
  p.hook = "h.py";
  hook_.actions["before_main"] = [](scripting::IMeasurementApi& api) -> Result<void> {
    if (auto r = api.position("Ar39", "CDD"); !r) return r;
    if (auto r = api.acquire(2, 1.0); !r) return r;
    if (auto r = api.open("X"); !r) return r;
    if (auto r = api.add_conditional("Ar40.cur > 0 -> truncate"); !r) return r;
    if (api.add_conditional("Ar40 > 0 -> run_blank")) return fail(ErrorKind::Config, "queue action accepted");
    api.log("hello");
    return {};
  };
  auto r = run(p);
  EXPECT_EQ(hook_.entries, (std::vector<std::string>{"before_main", "after_main"}));
  EXPECT_EQ(r.data.counts.at("hook"), 2);
  EXPECT_TRUE(valves_.state["X"]);
  EXPECT_EQ(r.notes, (std::vector<std::string>{"hello"}));
  // The hook's conditional truncates main on its first reading.
  EXPECT_EQ(r.outcome, MeasurementOutcome::Truncated);
  EXPECT_EQ(r.data.counts.at("main"), 1);
  // main re-positions after the hook moved the magnet.
  EXPECT_EQ(std::count(spec_.log.begin(), spec_.log.end(), "position Ar40@H1"), 2);
}

TEST_F(EngineTest, HookErrorFailsTheMeasurement) {
  auto p = multicollect(4);
  p.hook = "h.py";
  hook_.actions["after_main"] = [](scripting::IMeasurementApi&) -> Result<void> {
    return fail(ErrorKind::Config, "boom");
  };
  auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  EXPECT_EQ(r.error->what, "boom");
}

TEST_F(EngineTest, PlanTruncationsBecomeAConditionalLevel) {
  auto p = multicollect();
  p.conditionals.truncations = {{"Ar40 > 8e5", 20}};
  auto set = plan_conditionals(p);
  ASSERT_TRUE(set);
  ASSERT_EQ(set->items.size(), 1u);
  EXPECT_EQ(set->items[0].name, "plan.truncation[0]");
  EXPECT_EQ(set->items[0].kind, ConditionalKind::Truncation);
  EXPECT_EQ(set->items[0].start, 20);
  EXPECT_EQ(set->items[0].action.type, ActionSpec::Type::Truncate);
  p.conditionals.truncations = {{"Ar40 >", 1}};
  EXPECT_FALSE(plan_conditionals(p));
}

TEST_F(EngineTest, ResultsFitInterceptsAndBaselines) {
  auto p = multicollect(30);
  p.baseline.after = true;
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed);
  auto fits = fit_results(r.data, p.fits);
  EXPECT_TRUE(fits.errors.empty());
  // v = a (1 + 0.01 t) with t absolute clock seconds: the intercept at time
  // zero is a (1 + 0.01 t_zero).
  const double tz = secs(r.data.timing.epoch - TimePoint{}) + *r.data.timing.time_zero;
  ASSERT_TRUE(fits.results.intercepts.contains("Ar40"));
  EXPECT_NEAR(fits.results.intercepts.at("Ar40").intercept.value, 1000 * (1 + 0.01 * tz), 1e-6);
  EXPECT_NEAR(fits.results.intercepts.at("Ar39").intercept.value, 100 * (1 + 0.01 * tz), 1e-6);
  EXPECT_EQ(fits.results.intercepts.at("Ar40").fit.kind, reduction::FitKind::Linear);
  ASSERT_TRUE(fits.results.baselines.contains("H1"));
  EXPECT_DOUBLE_EQ(fits.results.baselines.at("H1").value, 0.0);  // magnet at mass 34.2: empty

  auto data = to_record_data(r.data);
  EXPECT_EQ(data.time_zero, *r.data.timing.time_zero);
  EXPECT_EQ(data.counts.at("main"), 30);
  bool found = false;
  for (const auto& s : data.series) {
    if (s.iso == "Ar40" && s.kind == "signal") {
      found = true;
      EXPECT_EQ(s.trace.t.size(), 30u);
      EXPECT_GT(s.trace.t.front(), 0.f);  // after time zero
    }
    if (s.kind == "sniff") {
      EXPECT_LT(s.trace.t.front(), 0.f);
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(EngineTest, IsotopeOnTwoDetectorsGetsDetectorQualifiedIntercepts) {
  collect::RunData d;
  d.series[{"Ar36", "CDD", SeriesKind::Signal}] = collect::Series{{1, 2, 3}, {1, 2, 3}, {}, {}};
  d.series[{"Ar36", "L2", SeriesKind::Signal}] = collect::Series{{1, 2, 3}, {2, 4, 6}, {}, {}};
  d.series[{"Ar40", "H1", SeriesKind::Signal}] = collect::Series{{1}, {5}, {}, {}};  // too few for linear
  auto out = fit_results(d, plan::Fits{});
  EXPECT_TRUE(out.results.intercepts.contains("Ar36:CDD"));
  EXPECT_TRUE(out.results.intercepts.contains("Ar36:L2"));
  EXPECT_FALSE(out.results.intercepts.contains("Ar40"));
  ASSERT_EQ(out.errors.size(), 1u);
  EXPECT_EQ(out.errors[0].substr(0, 5), "Ar40:");
}

}  // namespace
