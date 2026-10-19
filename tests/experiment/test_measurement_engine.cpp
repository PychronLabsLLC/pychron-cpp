// MeasurementEngine against fake ports on a ManualClock: block order, hop
// runner semantics, equilibration timing, time zero, conditionals,
// truncate/terminate/cancel/abort, safety on exit, hooks, and results.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/devices/link.hpp"
#include "pychron/experiment/measurement/results.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::measurement;
using namespace std::chrono_literals;
using collect::SeriesKey;
using collect::merged_series;

namespace {

double secs(pychron::Duration d) { return std::chrono::duration<double>(d).count(); }

SeriesKey skey(std::string label, std::string detector, std::string_view kind, int segment = 0, int position = 0) {
  SeriesKey k;
  k.label = std::move(label);
  k.detector = std::move(detector);
  k.kind = std::string(kind);
  k.segment = segment;
  k.position = position;
  return k;
}

// Intensity = abundance of the isotope on the detector at the magnet target,
// growing linearly with time since t0: v = a * (1 + 0.01 t).
class FakeSpectrometer final : public ISpectrometerPort {
 public:
  explicit FakeSpectrometer(ManualClock& clock) : clock_(clock) {}

  Result<void> position(const plan::At& target) override {
    log.push_back(describe(target));
    if (fail_position) return fail(ErrorKind::Io, "magnet fault");
    at_ = target;
    return {};
  }
  Result<double> set_deflection(const std::string& det, double value) override {
    log.push_back("deflect " + det + "=" + num(value));
    deflections[det] = value;
    return value;
  }
  Result<double> deflection(const std::string& det) override {
    const auto it = deflections.find(det);
    return it == deflections.end() ? 0.0 : it->second;
  }
  Result<void> with_table(const std::string& name) override {
    log.push_back("table " + name);
    return {};
  }
  Result<void> set_gain(const std::string& det, double gain) override {
    log.push_back("gain " + det + "=" + num(gain));
    return {};
  }
  Result<void> set_hv(double volts) override {
    log.push_back("hv " + num(volts));
    return {};
  }
  Result<void> set_cdd_voltage(double volts) override {
    log.push_back("cdd_voltage " + num(volts));
    return {};
  }
  Result<void> set_param(const std::string& name, double value) override {
    log.push_back("param " + name + "=" + num(value));
    return {};
  }
  Result<void> blank_beam(bool on) override {
    log.push_back(std::string("blank ") + (on ? "on" : "off"));
    return {};
  }
  Result<void> apply_optics(const std::string& name) override {
    log.push_back("optics " + name);
    return {};
  }
  Result<void> set_source_output(bool on) override {
    log.push_back(std::string("source_output ") + (on ? "on" : "off"));
    return {};
  }
  Result<void> protect(const std::string& det, bool on) override {
    if (protect_fault) {
      if (auto r = protect_fault(det, on); !r) return r;
    }
    log.push_back(std::string(on ? "protect " : "unprotect ") + det);
    if (on) protected_.insert(det); else protected_.erase(det);
    return {};
  }
  Result<void> start_acquisition(pychron::Duration integration) override {
    if (lose_starts > 0) {  // as NGX's StartAcq whose reply was lost (link.hpp)
      --lose_starts;
      log.emplace_back("start lost");
      return fail(Error{ErrorKind::Io, "StartAcq: reply lost", {}, kOutcomeUnknown});
    }
    log.push_back("start " + std::to_string(secs(integration)));
    integration_ = integration;
    acquiring = true;
    return {};
  }
  Result<std::optional<spectrometer::Reading>> next_reading(pychron::Duration timeout) override {
    if (!acquiring) return fail(ErrorKind::Config, "not acquiring");
    // Silent from silent_from until silent_until: the call waits its timeout
    // (or until the silence ends, when a reading comes at once), or fails at
    // once with Io when the link is down.
    if (silent_from && clock_.now() >= *silent_from && (!silent_until || clock_.now() < *silent_until)) {
      ++silent_calls;
      if (silence_is_link_down) {
        clock_.advance(link_down_costs);  // even a failing call takes some time on a real clock
        return fail(ErrorKind::Io, "spectrometer link down");
      }
      const TimePoint until = silent_until ? std::min(clock_.now() + timeout, *silent_until) : clock_.now() + timeout;
      clock_.set(until);
      if (!silent_until || clock_.now() < *silent_until) return std::optional<spectrometer::Reading>{};
      if (silence_ends_in_error) return fail(ErrorKind::Protocol, "acquirer fault");
    }
    ++readings;
    if (fail_at && readings == *fail_at) return fail(fail_kind, "acquirer stalled");
    clock_.advance(integration_);
    spectrometer::Reading r;
    r.ts = clock_.now() - ts_lag;
    r.integration = integration_;
    const double t = secs(clock_.now() - TimePoint{});
    for (const auto& det : detectors) {
      double v = 0.0;
      const auto* label = at_ ? std::get_if<plan::AtLabel>(&*at_) : nullptr;
      if (label != nullptr) {
        // The isotope sitting on `det` given where the reference was put.
        auto it = layout.find(label->label + "@" + label->detector);
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
    log.emplace_back("stop");
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
  ErrorKind fail_kind = ErrorKind::Timeout;
  std::optional<TimePoint> silent_from, silent_until;
  bool silence_is_link_down = false;
  pychron::Duration link_down_costs{};
  bool silence_ends_in_error = false;
  pychron::Duration ts_lag{};  // a reading's own time is this much behind the clock
  int silent_calls = 0;
  int lose_starts = 0;
  std::function<Result<void>(const std::string&, bool)> protect_fault;
  bool fail_position = false;
  bool acquiring = false;
  int readings = 0;
  std::set<std::string> protected_;
  std::map<std::string, double> deflections;  // detector -> current deflection; absent: 0

 private:
  static std::string num(double v) {
    std::ostringstream out;
    out << v;
    return out.str();
  }
  // "position Ar40@H1", "position 35.000000@CDD", "move_native 1.234",
  // "position_hv 4500@H1".
  static std::string describe(const plan::At& at) {
    if (const auto* l = std::get_if<plan::AtLabel>(&at)) return "position " + l->label + "@" + l->detector;
    if (const auto* m = std::get_if<plan::AtMass>(&at)) return "position " + std::to_string(m->mass) + "@" + m->detector;
    if (const auto* d = std::get_if<plan::AtDac>(&at)) return "move_native " + num(d->dac);
    const auto& h = std::get<plan::AtHv>(at);
    return "position_hv " + num(h.hv) + "@" + h.detector;
  }

  ManualClock& clock_;
  std::optional<plan::At> at_;
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
    if (close_fault) {
      if (auto r = close_fault(v); !r) return r;
    }
    log.push_back("close " + v + " @" + std::to_string(secs(clock_.now() - t0)));
    state[v] = false;
    return {};
  }
  std::vector<std::string> log;
  std::map<std::string, bool> state;
  TimePoint t0{};
  std::function<Result<void>(const std::string&)> close_fault;  // its error is the call's

 private:
  ManualClock& clock_;
};

class FakePeakCenter final : public IPeakCenterPort {
 public:
  Result<PeakCenterReport> peak_center(const PeakCenterRequest& req, scripting::CancelToken&) override {
    requests.push_back(req);
    PeakCenterReport report;
    report.request = req;
    report.ok = ok;
    if (ok) report.center = 8.0;
    else report.message = "no peak";
    return report;
  }
  void drop_run_tables() override { ++dropped; }
  std::vector<PeakCenterRequest> requests;
  bool ok = true;
  int dropped = 0;
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

// A plan as the engine tests write it: the v1 tables, rendered to TOML and
// resolved by the plan loader, so the engine runs the segments a v1 file
// desugars to (measurement plan v2 spec, 4.1).
struct TestHopPosition {
  std::string isotope, detector;
};

struct TestHop {
  std::map<std::string, std::string> positions;
  int counts = 0;
  double settle_s = 0;
  std::vector<std::string> protect;
  bool baseline = false;
  std::optional<double> mass;
  std::optional<TestHopPosition> position;
};

struct TestSniff {
  bool enabled = false;
  int counts = 0;
  double integration_s = 1;
};

struct TestPeakCenter {
  bool before = false, after = false;
  std::string detector, isotope, config = "default";
  double min_signal = 0;
};

struct TestBaseline {
  bool before = false, after = false;
  int counts = 0;
  std::optional<double> mass;
  std::string detector;
  double settle_s = 0;
  double integration_s = 1;
};

struct TestMain {
  int cycles = 1;
  double integration_s = 1;
  plan::TimeZero time_zero;
  std::vector<TestHop> hops;
};

struct TestWhiff {
  bool enabled = false;
  int counts = 0;
  double integration_s = 1;
  std::vector<plan::WhiffCheck> checks;
};

std::string num(double v) {
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%.17g", v);
  return buf.data();
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

// NOLINTNEXTLINE(modernize-use-string-view): concatenated with "+" into TOML text, which a string_view does not take
std::string boolean(bool b) { return b ? "true" : "false"; }

std::string string_array(const std::vector<std::string>& v) {
  std::string out = "[";
  for (std::size_t i = 0; i < v.size(); ++i) out += (i == 0 ? "" : ", ") + quoted(v[i]);
  return out + "]";
}

// The plan through the loader, which fails the test when it does not resolve.
plan::MeasurementPlan plan_from(std::string_view toml) {
  auto r = plan::resolve_plan(toml, plan::PlanResolvers{}, "test");
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().what) << "\n" << toml;
  return r.value_or(plan::MeasurementPlan{});
}

struct V1Plan {
  plan::DetectorsSpec detectors;
  plan::Equilibration equilibration;
  TestSniff sniff;
  TestPeakCenter peak_center;
  TestBaseline baseline;
  TestMain main;
  TestWhiff whiff;
  std::optional<std::string> hook;

  std::string toml() const {
    std::string t;
    if (hook) t += "hook = " + quoted(*hook) + "\n";
    t += "[plan]\nname = \"mc\"\ninstrument_family = \"fake\"\n";
    t += "[detectors]\nreference = " + quoted(detectors.reference) + "\nexclude = " + string_array(detectors.exclude) + "\n";
    const auto& eq = equilibration;
    t += "[equilibration]\ninlet = " + quoted(eq.inlet) + "\noutlet = " + quoted(eq.outlet) + "\ntime_s = " + num(eq.time_s) +
         "\ninlet_delay_s = " + num(eq.inlet_delay_s) + "\nclose_inlet = " + boolean(eq.close_inlet) + "\n";
    t += "[sniff]\nenabled = " + boolean(sniff.enabled) + "\ncounts = " + std::to_string(sniff.counts) +
         "\nintegration_s = " + num(sniff.integration_s) + "\n";
    const auto& pc = peak_center;
    t += "[peak_center]\nbefore = " + boolean(pc.before) + "\nafter = " + boolean(pc.after) + "\ndetector = " +
         quoted(pc.detector) + "\nisotope = " + quoted(pc.isotope) + "\nconfig = " + quoted(pc.config) +
         "\nmin_signal = " + num(pc.min_signal) + "\n";
    const auto& b = baseline;
    t += "[baseline]\nbefore = " + boolean(b.before) + "\nafter = " + boolean(b.after) + "\ncounts = " +
         std::to_string(b.counts) + "\ndetector = " + quoted(b.detector) + "\nsettle_s = " + num(b.settle_s) +
         "\nintegration_s = " + num(b.integration_s) + "\n";
    if (b.mass) t += "mass = " + num(*b.mass) + "\n";
    t += "[main]\ncycles = " + std::to_string(main.cycles) + "\nintegration_s = " + num(main.integration_s) + "\n";
    switch (main.time_zero.kind) {
      case plan::TimeZeroKind::OnInletClose: t += "time_zero = \"on_inlet_close\"\n"; break;
      case plan::TimeZeroKind::OnFirstCount: t += "time_zero = \"on_first_count\"\n"; break;
      default: t += "time_zero = { offset_s = " + num(main.time_zero.offset_s) + " }\n"; break;
    }
    for (const auto& h : main.hops) {
      t += "[[main.hops]]\npositions = {";
      bool first = true;
      for (const auto& [iso, det] : h.positions) {
        t += std::string(first ? " " : ", ") + iso + " = " + quoted(det);
        first = false;
      }
      t += " }\ncounts = " + std::to_string(h.counts) + "\nsettle_s = " + num(h.settle_s) + "\nprotect = " +
           string_array(h.protect) + "\nbaseline = " + boolean(h.baseline) + "\n";
      if (h.mass) t += "mass = " + num(*h.mass) + "\n";
      if (h.position) {
        t += "position = { detector = " + quoted(h.position->detector);
        if (!h.position->isotope.empty()) t += ", isotope = " + quoted(h.position->isotope);
        t += " }\n";
      }
    }
    t += "[whiff]\nenabled = " + boolean(whiff.enabled) + "\ncounts = " + std::to_string(whiff.counts) +
         "\nintegration_s = " + num(whiff.integration_s) + "\n";
    for (const auto& c : whiff.checks)
      t += "[[whiff.checks]]\ncheck = " + quoted(c.check) + "\naction = " + quoted(c.action) + "\n";
    return t;
  }

  plan::MeasurementPlan plan() const { return plan_from(toml()); }
};

TestHop hop(std::map<std::string, std::string> positions, int counts, double settle = 2) {
  TestHop h;
  h.positions = std::move(positions);
  h.counts = counts;
  h.settle_s = settle;
  return h;
}

V1Plan multicollect(int counts = 10, int cycles = 1) {
  V1Plan p;
  p.detectors.reference = "H1";
  p.equilibration = {"B", "C", 10, 2, true};
  p.sniff = {true, 10, 1};
  p.baseline = {false, false, 5, 34.2, "H1", 3, 1};
  p.main.cycles = cycles;
  p.main.integration_s = 1;
  p.main.hops.push_back(hop({{"Ar40", "H1"}, {"Ar39", "AX"}, {"Ar36", "CDD"}}, counts));
  return p;
}

ActionSpec action(ActionSpec::Type type, std::string name = {}) {
  ActionSpec a;
  a.type = type;
  a.name = std::move(name);
  return a;
}

MeasurementInputs inputs(plan::MeasurementPlan p, ConditionalSet conditionals = {}) {
  MeasurementInputs in;
  in.plan = std::move(p);
  in.conditionals = std::move(conditionals);
  in.run_id = "run-1";
  return in;
}

MeasurementInputs inputs(const V1Plan& p, ConditionalSet conditionals = {}) {
  return inputs(p.plan(), std::move(conditionals));
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

  MeasurementResult run(const V1Plan& p, ConditionalSet conditionals = {}, EngineOptions options = {}) {
    return run(p.plan(), std::move(conditionals), std::move(options));
  }

  MeasurementResult run(plan::MeasurementPlan p, ConditionalSet conditionals = {}, EngineOptions options = {}) {
    MeasurementInputs in = inputs(std::move(p), std::move(conditionals));
    if (!options.sleep) options.sleep = [this](pychron::Duration d) { clock_.advance(d); };
    engine_ = std::make_unique<MeasurementEngine>(context(), std::move(in), std::move(options));
    return engine_->run(token_);
  }

  EngineContext context() {
    EngineContext c{spec_, clock_, &valves_, &peak_, &hook_, &bus_, nullptr};
    c.timing = timing_;
    return c;
  }

  // Points of a label on a detector, wherever in the plan they were read.
  static int count(const MeasurementResult& r, std::string_view label, std::string_view detector,
                   std::string_view kind) {
    const auto s = merged_series(r.data, label, detector, kind);
    return s ? static_cast<int>(s->v.size()) : 0;
  }

  TimePoint t0_ = TimePoint{} + 1000s;
  std::map<std::string, DetectorTiming> timing_;  // EngineContext::timing
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
  p.peak_center = {true, true, "", "Ar40", "default", 0};
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
  EXPECT_EQ(count(r, "", "H1", collect::kBaseline), 10);
  EXPECT_EQ(count(r, "", "CDD", collect::kBaseline), 10);
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 10);
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
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSniff), 10);
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
  const auto s = merged_series(r.data, "Ar40", "H1", collect::kSignal).value();
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
    if (key.kind == collect::kSignal) {
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
  h1.position = TestHopPosition{"Ar39", "CDD"};
  auto h2 = hop({{"Ar36", "CDD"}}, 1);
  h2.baseline = true;
  h2.mass = 34.2;
  h2.position = TestHopPosition{"", "CDD"};
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
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 6);
  EXPECT_EQ(count(r, "Ar36", "CDD", collect::kSignal), 6);
  EXPECT_EQ(count(r, "Ar39", "CDD", collect::kSignal), 4);
  EXPECT_EQ(count(r, "Ar36", "CDD", collect::kBaseline), 2);
  EXPECT_EQ(r.data.counts.at("main"), 12);
  EXPECT_TRUE(spec_.protected_.empty());
  // Ar39 on CDD reads Ar39's abundance, not Ar36's.
  EXPECT_NEAR(merged_series(r.data, "Ar39", "CDD", collect::kSignal).value().v.front() /
                  merged_series(r.data, "Ar36", "CDD", collect::kSignal).value().v.front(),
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
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 11);  // first check after reading start + 1
  EXPECT_DOUBLE_EQ(r.count_scale, 0.3);                         // the conditional's ratio (pychron)
  EXPECT_EQ(r.data.counts.at("baseline.after"), 9);  // ceil(30 * 0.3)
  ASSERT_EQ(r.data.trips.size(), 1u);
  EXPECT_EQ(r.data.trips[0].name, "big");
}

// ---- set, blank_beam, park, delay, time_zero steps (plan Task 17) ----------------

std::string steps_plan(std::string_view before, std::string_view after = "") {
  return std::string(R"toml(
[plan]
name = "steps"
instrument_family = "thermo_argus"
analysis_types = ["unknown"]

[equilibration]
inlet = "B"
outlet = "C"
time_s = 10
)toml") + std::string(before) + R"toml(
[[segment]]
kind = "signal"
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 3
)toml" + std::string(after);
}

// The entries of `log` from the first that starts with `from`.
std::vector<std::string> log_from(const std::vector<std::string>& log, std::string_view from) {
  const auto it = std::find_if(log.begin(), log.end(), [&](const std::string& l) { return l.starts_with(from); });
  return {it, log.end()};
}

TEST_F(EngineTest, StepsSetMapsEachSubKeyToOneCall) {
  auto r = run(plan_from(steps_plan(R"(
[[segment]]
kind = "set"
when = "before_inlet"
set = { deflection = { CDD = 2000, H1 = 10 }, gain = { H1 = 1.5 }, source = { trap_current = 200 }, hv = 4500, cdd_voltage = 1800, integration_s = 2 }
)")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  const auto from = log_from(spec_.log, "deflect CDD");
  ASSERT_GE(from.size(), 6u);
  EXPECT_EQ(std::vector<std::string>(from.begin(), from.begin() + 6),
            (std::vector<std::string>{"deflect CDD=2000", "deflect H1=10", "gain H1=1.5", "param trap_current=200",
                                      "hv 4500", "cdd_voltage 1800"}));
  // integration_s holds for the gas's later collections.
  EXPECT_NE(std::find(spec_.log.begin(), spec_.log.end(), "start 2.000000"), spec_.log.end());
  // What the step left standing goes back when the run ends.
  const auto end = log_from(spec_.log, "deflect CDD=0");
  EXPECT_FALSE(end.empty());
  EXPECT_NE(std::find(end.begin(), end.end(), "deflect H1=0"), end.end());
}

TEST_F(EngineTest, StepsBlankBeam) {
  auto r = run(plan_from(steps_plan(R"(
[[segment]]
kind = "blank_beam"
when = "before_inlet"
blank_beam = true
)", R"(
[[segment]]
kind = "blank_beam"
when = "after_main"
blank_beam = false
)")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(std::count(spec_.log.begin(), spec_.log.end(), "blank on"), 1);
  EXPECT_EQ(std::count(spec_.log.begin(), spec_.log.end(), "blank off"), 1);
  // A beam left blanked is let through when the run ends.
  spec_.log.clear();
  auto left = run(plan_from(steps_plan(R"(
[[segment]]
kind = "blank_beam"
when = "before_inlet"
blank_beam = true
)")));
  ASSERT_EQ(left.outcome, MeasurementOutcome::Completed);
  EXPECT_EQ(spec_.log.back(), "blank off");
}

TEST_F(EngineTest, StepsParkMovesWithoutCollecting) {
  auto r = run(plan_from(steps_plan("", R"(
[[segment]]
kind = "park"
when = "after_main"
park = { mass = 36.5, detector = "CDD" }
)")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(spec_.log.back(), "position 36.500000@CDD");
  int collections = 0;
  for (const auto& e : r.trace) collections += e.what == "collect" ? 1 : 0;
  EXPECT_EQ(collections, 1);  // the signal's only
}

TEST_F(EngineTest, StepsDelayWaitsOnTheClock) {
  const auto plain_run = run(plan_from(steps_plan("")));
  const auto plain_end = clock_.now();
  ASSERT_EQ(plain_run.outcome, MeasurementOutcome::Completed);
  clock_.set(t0_);
  auto r = run(plan_from(steps_plan("", R"(
[[segment]]
kind = "delay"
when = "after_main"
delay = { seconds = 42 }
)")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(clock_.now() - plain_end, std::chrono::seconds(42));
}

// Worked plan 7.3's time zero after its IC block (at after_main here: an
// after_inlet step runs with Task 19's phases): the clock at the step, not
// the inlet's close.
TEST_F(EngineTest, StepsTimeZeroStepAfterIcBlock) {
  auto r = run(plan_from(steps_plan("", R"(
[[segment]]
kind = "time_zero"
when = "after_main"
time_zero = "now"
)")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_TRUE(r.data.timing.time_zero);
  ASSERT_TRUE(r.data.timing.inlet_close);
  const auto& signal = r.data.series.begin()->second;
  EXPECT_GT(*r.data.timing.time_zero, *r.data.timing.inlet_close);
  EXPECT_GE(*r.data.timing.time_zero, signal.t.back());  // the IC block is before it
  EXPECT_DOUBLE_EQ(*r.data.timing.time_zero, secs(clock_.now() - r.data.timing.epoch));
}

// ---- phases (plan Task 19) ----------------------------------------------------

std::string phases_plan(std::string_view segments, double eq_time = 10) {
  return std::string(R"toml(
[plan]
name = "phases"
instrument_family = "thermo_argus"
analysis_types = ["unknown"]

[equilibration]
inlet = "B"
outlet = "C"
time_s = )toml") + num(eq_time) + "\n" + std::string(segments);
}

// Segment indexes of the trace's collections and steps, in the order run
// (one entry per segment however often it collected).
std::vector<int> run_order(const MeasurementResult& r) {
  std::vector<int> out;
  for (const auto& e : r.trace) {
    if (e.what != "collect" && e.what != "step") continue;
    if (out.empty() || out.back() != e.segment) out.push_back(e.segment);
  }
  return out;
}

// The time (s since the epoch) of the first trace entry of `segment`.
double first_t(const MeasurementResult& r, int segment) {
  for (const auto& e : r.trace)
    if (e.segment == segment) return e.t;
  return -1;
}

TEST_F(EngineTest, PhasesOrderAcrossWhens) {
  auto r = run(plan_from(phases_plan(R"(
[[segment]]
kind = "delay"
when = "after_run"
delay = { seconds = 1 }

[[segment]]
kind = "baseline"
when = "after_main"
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 34.2, detector = "H1" }
counts = 2

[[segment]]
kind = "signal"
when = "after_inlet"
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 3

[[segment]]
kind = "baseline"
when = "during_equilibration"
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 35, detector = "H1" }
counts = 2

[[segment]]
kind = "delay"
when = "before_inlet"
delay = { seconds = 1 }
)")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(run_order(r), (std::vector<int>{4, 3, 2, 1, 0}));
  // during_equilibration runs once the inlet is open, after_inlet once it closed.
  ASSERT_TRUE(r.data.timing.inlet_open && r.data.timing.inlet_close);
  EXPECT_GE(first_t(r, 3), *r.data.timing.inlet_open);
  EXPECT_GE(first_t(r, 2), *r.data.timing.inlet_close);
  EXPECT_EQ(r.blocks, (std::vector<Block>{Block::Step, Block::PositionFirstHop, Block::Equilibrate, Block::Segment,
                                          Block::Main, Block::BaselineAfter, Block::Step}));
}

TEST_F(EngineTest, PhasesSameWhenRunsInFileOrder) {
  auto r = run(plan_from(phases_plan(R"(
[[segment]]
kind = "signal"
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 3

[[segment]]
kind = "baseline"
when = "after_main"
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 35, detector = "H1" }
counts = 2

[[segment]]
kind = "delay"
when = "after_main"
delay = { seconds = 2 }

[[segment]]
kind = "baseline"
when = "after_main"
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 34.2, detector = "H1" }
counts = 2
)")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(run_order(r), (std::vector<int>{0, 1, 2, 3}));
}

// after_main comes after every after_inlet signal segment, wherever it is
// written.
TEST_F(EngineTest, PhasesAfterMainAfterLastSignalSegment) {
  auto r = run(plan_from(phases_plan(R"(
[[segment]]
kind = "signal"
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 3

[[segment]]
kind = "baseline"
when = "after_main"
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 35, detector = "H1" }
counts = 2

[[segment]]
kind = "signal"
[[segment.positions]]
collect = { Ar39 = "H1" }
counts = 3
)")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(run_order(r), (std::vector<int>{0, 2, 1}));
}

// A baseline while the gas equilibrates: its positions are read while the
// equilibration waits, and the inlet closes on time, at a reading.
TEST_F(EngineTest, PhasesBaselineDuringEquilibration) {
  auto r = run(plan_from(phases_plan(R"(
[[segment]]
kind = "baseline"
when = "during_equilibration"
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 35, detector = "H1" }
counts = 8
settle_s = 1

[[segment]]
kind = "signal"
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 3
)", 4)));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_TRUE(r.data.timing.inlet_open && r.data.timing.inlet_close);
  const double held = *r.data.timing.inlet_close - *r.data.timing.inlet_open;
  EXPECT_GE(held, 4.0);
  EXPECT_LE(held, 5.0);  // within a reading of its time
  const auto& baseline = r.data.series.at(skey("", "H1", collect::kBaseline, 0, 0));
  EXPECT_EQ(baseline.v.size(), 8u);  // read to its end, past the close
  EXPECT_GT(baseline.t.back(), *r.data.timing.inlet_close);
  EXPECT_LT(baseline.t.front(), *r.data.timing.inlet_close);
  EXPECT_EQ(std::count_if(valves_.log.begin(), valves_.log.end(),
                          [](const std::string& l) { return l.starts_with("close B"); }),
            1);
  EXPECT_DOUBLE_EQ(*r.data.timing.time_zero, *r.data.timing.inlet_close);
}

// ---- live edits (plan Task 15) -------------------------------------------------

constexpr std::string_view kTwoSegmentPlan = R"toml(
[plan]
name = "live"
instrument_family = "thermo_argus"
analysis_types = ["unknown"]

[detectors]
reference = "H1"

[equilibration]
inlet = "B"
outlet = "C"
time_s = 10

[[segment]]
kind = "signal"
cycles = 5
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 2
[[segment.positions]]
collect = { Ar39 = "H1" }
counts = 3

[[segment]]
kind = "signal"
cycles = 5
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 2
)toml";

// Points a segment collected, over every position and label.
int points_in(const MeasurementResult& r, int segment) {
  int n = 0;
  for (const auto& [key, s] : r.data.series)
    if (key.segment == segment && key.kind == collect::kSignal) n += static_cast<int>(s.v.size());
  return n;
}

// Review Focus 5: a live `cycles` changes the running segment only.
TEST_F(EngineTest, LiveEditCyclesChangeOnlyTheRunningSegment) {
  spec_.on_reading = [&](int n) {
    if (n == 1) engine_->set_cycles(2);
  };
  auto r = run(plan_from(kTwoSegmentPlan));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(points_in(r, 0), 2 * (2 + 3));
  EXPECT_EQ(points_in(r, 1), 5 * 2);
}

// Fewer cycles than the one in progress: it is finished, no more.
TEST_F(EngineTest, LiveEditCyclesNeverCutTheCycleInProgress) {
  spec_.on_reading = [&](int n) {
    if (n == 12) engine_->set_cycles(1);  // in cycle 3 (readings 11 to 15)
  };
  auto r = run(plan_from(kTwoSegmentPlan));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(points_in(r, 0), 3 * (2 + 3));
  EXPECT_EQ(points_in(r, 1), 5 * 2);
}

// A live `counts` changes the running position's collection only: the next
// position, and the same position next cycle, collect as planned.
TEST_F(EngineTest, LiveEditCountsChangeOnlyTheRunningPosition) {
  spec_.on_reading = [&](int n) {
    if (n == 1) engine_->set_target(4);
  };
  auto r = run(plan_from(kTwoSegmentPlan));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  const auto first = r.data.series.find(skey("Ar40", "H1", collect::kSignal, 0, 0));
  ASSERT_NE(first, r.data.series.end());
  EXPECT_EQ(first->second.v.size(), 4u + 4 * 2);  // cycle 1 took 4, cycles 2 to 5 their 2
  EXPECT_EQ(r.data.series.at(skey("Ar39", "H1", collect::kSignal, 0, 1)).v.size(), 5u * 3);
  EXPECT_EQ(points_in(r, 1), 5 * 2);
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
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 5);  // requested while reading 5 was in flight
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
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 7);
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
                                  action(ActionSpec::Type::SkipAliquot)));
  // First trip wins: a resuming action that stays true would starve the
  // actions after it, so this one is true at reading 4 only.
  set.items.push_back(conditional(ConditionalKind::Action, "note", "count(Ar40) == 4", 3,
                                  action(ActionSpec::Type::Notify)));
  set.items.back().resume = true;
  set.items.push_back(conditional(ConditionalKind::Action, "hook", "count(Ar40) == 5", 4,
                                  action(ActionSpec::Type::RunHook, "on_big")));
  set.items.back().resume = true;
  set.items.push_back(conditional(ConditionalKind::Action, "cut", "Ar40.cur > 0", 6,
                                  action(ActionSpec::Type::Truncate)));
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Truncated);
  ASSERT_EQ(r.modifications.size(), 1u);
  EXPECT_EQ(r.modifications[0].action.type, ActionSpec::Type::SkipAliquot);
  EXPECT_EQ(r.modifications[0].name, "skip");
  EXPECT_EQ(r.notes, (std::vector<std::string>{"notify: note"}));
  EXPECT_EQ(hook_.entries, (std::vector<std::string>{"before_main", "on_big", "after_main"}));
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 7);
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
                                  action(ActionSpec::Type::Notify)));
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed);
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 4);  // later cycles skipped
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
                                  action(ActionSpec::Type::RunBlank)));
  set.items.back().truncate = true;
  set.items.back().abbreviated_count_ratio = 0.5;
  auto r = run(p, set);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Truncated);
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 5);
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
  MeasurementInputs in = inputs(p, set);
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
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kWhiff), 3);
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSniff), 7);  // the rest of the 10 s equilibration
  // The whiff happens after the inlet opens.
  EXPECT_GT(merged_series(r.data, "Ar40", "H1", collect::kWhiff).value().t.front(), *r.data.timing.inlet_open);

  p.whiff.checks = {{"Ar40.cur > 500", "pump"}};
  valves_.log.clear();
  r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Terminated);
  EXPECT_EQ(r.whiff, WhiffCheck::Action::Pump);
  EXPECT_FALSE(valves_.state["B"]);
  EXPECT_TRUE(valves_.state["C"]);  // outlet opened to pump the gas away
  EXPECT_EQ(r.blocks.back(), Block::Equilibrate);
  EXPECT_FALSE(merged_series(r.data, "Ar40", "H1", collect::kSignal).has_value());

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
  EXPECT_LT(count(r, "Ar40", "H1", collect::kSignal), 40);
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
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 3);
}

TEST_F(EngineTest, AcquisitionFailureFailsAndCleansUp) {
  auto p = multicollect(10);
  p.equilibration.close_inlet = false;
  spec_.fail_at = 15;  // inside main (10 sniff readings first)
  spec_.fail_kind = ErrorKind::Protocol;  // definite: a timeout is a gap (MeasurementGap below)
  auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_EQ(r.error->kind, ErrorKind::Protocol);
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
  p.peak_center = {true, false, "AX", "Ar39", "default", 0};
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

// A peak center after a blank would fit whatever small peak is left and move
// the field table by it: below the plan's minimum the block does nothing.
TEST_F(EngineTest, APeakCenterAfterIsSkippedWhenTheSignalIsBelowThePlansMinimum) {
  auto p = multicollect(12);
  p.sniff.enabled = false;
  p.peak_center.after = true;
  p.peak_center.isotope = "Ar39";
  p.peak_center.detector = "AX";
  p.peak_center.min_signal = 5000;  // Ar39 reads about 1100 here, Ar40 about 11000
  std::vector<std::string> said;
  auto sub = bus_.subscribe<Log>([&](const Log& l) { said.push_back(l.message); });
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_TRUE(peak_.requests.empty());
  ASSERT_EQ(r.peak_centers.size(), 1u);
  EXPECT_TRUE(r.peak_centers[0].skipped);
  EXPECT_FALSE(r.peak_centers[0].ok);
  EXPECT_FALSE(r.peak_centers[0].table_updated);
  EXPECT_EQ(r.peak_centers[0].request.isotope, "Ar39");
  EXPECT_EQ(r.peak_centers[0].request.detector, "AX");
  EXPECT_NE(r.peak_centers[0].message.find("below min_signal 5000"), std::string::npos) << r.peak_centers[0].message;
  EXPECT_EQ(r.blocks.back(), Block::PeakCenterAfter);
  const auto told = std::ranges::count_if(said, [](const std::string& m) { return m.find("skipped") != std::string::npos; });
  EXPECT_EQ(told, 1);
}

// ---- the peak_center step (plan Task 16) ------------------------------------------

std::string ic_plan(std::string_view peak_center, std::string_view when = "before_inlet") {
  const std::string table = when == "before_inlet" ? "table = \"ic_run\"\n" : "";
  return std::string(R"toml(
[plan]
name = "ic"
instrument_family = "thermo_argus"
analysis_types = ["unknown"]

[equilibration]
inlet = "B"
outlet = "C"
time_s = 10

[[segment]]
kind = "peak_center"
when = ")toml") + std::string(when) + R"toml("
peak_center = )toml" + std::string(peak_center) + R"toml(

[[segment]]
kind = "signal"
cycles = 2
)toml" + table + R"toml(
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 3
[[segment.positions]]
collect = { Ar40 = "AX" }
counts = 3
)toml";
}

// Worked plan 7.3: the job runs once per detector, each into the run's table.
TEST_F(EngineTest, PeakCenterStepOncePerDetector) {
  auto r = run(plan_from(ic_plan(R"({ label = "Ar40", detectors = ["H1", "AX", "L2"], write_table = "ic_run" })")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_EQ(peak_.requests.size(), 3u);
  const std::vector<std::string> dets{"H1", "AX", "L2"};
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(peak_.requests[i].detector, dets[i]);
    EXPECT_EQ(peak_.requests[i].isotope, "Ar40");
    EXPECT_EQ(peak_.requests[i].write_table, "ic_run");
  }
  EXPECT_EQ(r.peak_centers.size(), 3u);
  // The positions then run on the run's table.
  EXPECT_EQ(std::count(spec_.log.begin(), spec_.log.end(), "table ic_run"), 1);
  EXPECT_EQ(peak_.dropped, 1);  // the run's tables end with it
}

// Worked plan 7.1: extra detectors are fitted with the reference.
TEST_F(EngineTest, PeakCenterStepExtraDetectorsPassed) {
  auto r = run(plan_from(ic_plan(R"({ label = "Ar40", detector = "H1", extra_detectors = ["AX", "CDD"], write_table = "ic_run" })")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_EQ(peak_.requests.size(), 1u);
  EXPECT_EQ(peak_.requests[0].extra_detectors, (std::vector<std::string>{"AX", "CDD"}));
}

// The v1 rule stays: an after_run peak center under min_signal is skipped.
TEST_F(EngineTest, PeakCenterStepMinSignalSkipsAfterRun) {
  auto r = run(plan_from(ic_plan(R"({ label = "Ar40", detector = "H1", min_signal = 1e12, write_table = "ic_run" })",
                                 "after_run")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_TRUE(peak_.requests.empty());
  ASSERT_EQ(r.peak_centers.size(), 1u);
  EXPECT_TRUE(r.peak_centers[0].skipped);
}

// Review Focus 1: a failed write leaves the positions that name the table on
// the current one; the run says so and goes on.
TEST_F(EngineTest, PeakCenterStepFailedWriteTableFallsBackToCurrentTable) {
  peak_.ok = false;
  auto r = run(plan_from(ic_plan(R"({ label = "Ar40", detectors = ["H1", "AX"], write_table = "ic_run" })")));
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(peak_.requests.size(), 2u);
  EXPECT_EQ(std::count(spec_.log.begin(), spec_.log.end(), "table ic_run"), 0);
  const auto notes = std::count(r.notes.begin(), r.notes.end(), "table ic_run not written: no peak");
  EXPECT_EQ(notes, 1);  // once, not once per detector
  EXPECT_EQ(merged_series(r.data, "Ar40", "AX", collect::kSignal).value().v.size(), 6u);
}

// The signal compared is the mean of the last ten readings of the main block.
TEST_F(EngineTest, APeakCenterAfterRunsWhenTheSignalReachesTheMinimum) {
  auto p = multicollect(12);
  p.sniff.enabled = false;
  p.peak_center.after = true;
  p.peak_center.isotope = "Ar40";
  auto plain = run(p);
  const auto v = merged_series(plain.data, "Ar40", "H1", collect::kSignal).value().v;
  ASSERT_EQ(v.size(), 12u);
  double last_ten = 0;
  for (std::size_t i = 2; i < v.size(); ++i) last_ten += v[i] / 10;
  ASSERT_LT(v.front(), v.back());  // the fake's signal grows: the mean of all twelve is lower

  peak_.requests.clear();
  p.peak_center.min_signal = last_ten;
  auto at = run(p);
  ASSERT_EQ(at.peak_centers.size(), 1u);
  EXPECT_FALSE(at.peak_centers[0].skipped);
  EXPECT_TRUE(at.peak_centers[0].ok);
  EXPECT_EQ(peak_.requests.size(), 1u);
}

// Before the gas is in there is no reading to compare: the block runs.
TEST_F(EngineTest, APeakCenterBeforeIsNotHeldToTheMinimumSignal) {
  auto p = multicollect(3);
  p.sniff.enabled = false;
  p.peak_center.before = true;
  p.peak_center.isotope = "Ar40";
  p.peak_center.min_signal = 1e9;
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed);
  ASSERT_EQ(r.peak_centers.size(), 1u);
  EXPECT_FALSE(r.peak_centers[0].skipped);
  EXPECT_EQ(peak_.requests.size(), 1u);
}

TEST_F(EngineTest, ValidateNamesMissingServices) {
  auto p = multicollect();
  p.peak_center.after = true;
  p.peak_center.isotope = "Ar40";
  p.hook = "h.py";
  EngineContext ctx{spec_, clock_};
  MeasurementEngine e(ctx, inputs(p));
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

// Hooks as capable as the plan (spec 5.5, plan Task 18).
TEST_F(EngineTest, EngineHookApiNewCallsReachThePorts) {
  auto p = multicollect(4);
  p.sniff.enabled = false;
  p.hook = "h.py";
  hook_.actions["before_main"] = [](scripting::IMeasurementApi& api) -> Result<void> {
    if (auto r = api.peak_center("Ar40", "AX", "ic_run"); !r) return r;
    if (auto r = api.set_deflection("CDD", 110); !r) return r;
    if (auto r = api.set_integration(2); !r) return r;
    if (auto r = api.with_table("ic"); !r) return r;
    if (auto r = api.apply_optics("ar_config_a"); !r) return r;
    if (api.set_integration(0)) return fail(ErrorKind::Config, "zero integration accepted");
    return {};
  };
  auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_EQ(peak_.requests.size(), 1u);
  EXPECT_EQ(peak_.requests[0].isotope, "Ar40");
  EXPECT_EQ(peak_.requests[0].detector, "AX");
  EXPECT_EQ(peak_.requests[0].write_table, "ic_run");
  const auto from = log_from(spec_.log, "deflect CDD=110");
  ASSERT_GE(from.size(), 3u);
  EXPECT_EQ(std::vector<std::string>(from.begin(), from.begin() + 3),
            (std::vector<std::string>{"deflect CDD=110", "table ic", "optics ar_config_a"}));
  // Main collects with the hook's integration; the deflection stands to the end.
  EXPECT_NE(std::find(from.begin(), from.end(), "start 2.000000"), from.end());
  EXPECT_EQ(spec_.log.back(), "deflect CDD=0");
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
  plan::MeasurementPlan p;
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
  auto fits = fit_results(r.data, p.plan());
  EXPECT_TRUE(fits.errors.empty());
  // v = a (1 + 0.01 t) with t absolute clock seconds: the intercept at time
  // zero is a (1 + 0.01 t_zero).
  const double tz = secs(r.data.timing.epoch - TimePoint{}) + *r.data.timing.time_zero;
  ASSERT_TRUE(fits.results.intercepts.contains("Ar40@H1"));
  EXPECT_NEAR(fits.results.intercepts.at("Ar40@H1").intercept.value, 1000 * (1 + 0.01 * tz), 1e-6);
  EXPECT_NEAR(fits.results.intercepts.at("Ar39@AX").intercept.value, 100 * (1 + 0.01 * tz), 1e-6);
  EXPECT_EQ(fits.results.intercepts.at("Ar40@H1").fit.kind, reduction::FitKind::Linear);
  ASSERT_TRUE(fits.results.baselines.contains("H1"));
  EXPECT_DOUBLE_EQ(fits.results.baselines.at("H1").value, 0.0);  // magnet at mass 34.2: empty

  auto data = to_record_data(r.data, p.plan());
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

// The record (v3) keeps a series per position; one label on one detector is
// one intercept, its positions fitted as one series in time order.
TEST_F(EngineTest, RecordKeepsPositionsAndFitsOneLabelOnOneDetectorAsOne) {
  collect::RunData d;
  d.series[skey("Ar40", "H1", collect::kSignal, 1, 0)] = collect::Series{{1, 3}, {10, 30}, {}, {false, false}};
  d.series[skey("Ar40", "H1", collect::kSignal, 1, 1)] = collect::Series{{2, 4}, {20, 40}, {}, {false, false}};
  d.series[skey("", "H1", collect::kBaseline, 0, 0)] = collect::Series{{0}, {1}, {}, {false}};
  d.series[skey("", "H1", collect::kBaseline, 2, 0)] = collect::Series{{5}, {3}, {}, {false}};
  const auto data = to_record_data(d, plan::MeasurementPlan{});
  ASSERT_EQ(data.series.size(), 4u);
  // By label, as v2 records were: the baseline segment's (no label) first,
  // then by segment and position.
  EXPECT_EQ(data.series[0].kind, "baseline");
  EXPECT_EQ(data.series[0].segment, 0);
  EXPECT_EQ(data.series[1].segment, 2);
  EXPECT_EQ(data.series[2].iso, "Ar40");
  EXPECT_EQ(data.series[2].position, 0);
  EXPECT_EQ(data.series[3].position, 1);
  EXPECT_EQ(data.series[3].trace.v, (std::vector<float>{20, 40}));
  ASSERT_EQ(data.gases.size(), 1u);
  const auto out = fit_results(d, plan::MeasurementPlan{});
  EXPECT_EQ(out.results.intercepts.size(), 1u);
  EXPECT_NEAR(out.results.intercepts.at("Ar40@H1").intercept.value, 0, 1e-9);  // y = 10 t
  ASSERT_EQ(out.results.baseline_positions.size(), 2u);
  // The two baselines bracket the signal: their average.
  EXPECT_DOUBLE_EQ(out.results.baselines.at("H1").value, 2);
  EXPECT_EQ(out.results.baseline_choices.at("Ar40@H1").rule, "bracketing_average");
}

// ---- the baseline rule (spec 6.2) ---------------------------------------------

constexpr std::string_view kBaselineRulePlan = R"toml(
[plan]
name = "baseline_rule"
instrument_family = "thermo_argus"
analysis_types = ["unknown"]

[detectors]
reference = "H1"

[equilibration]
inlet = "B"
outlet = "C"
time_s = 10

[[segment]]
kind = "signal"
when = "after_inlet"
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 3
[[segment.positions]]
kind = "baseline"
collect = { Ar40 = "H1" }
counts = 2

[[segment]]
kind = "baseline"
when = "before_inlet"
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 34.2, detector = "H1" }
counts = 2

[[segment]]
kind = "baseline"
when = "after_main"
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { mass = 34.2, detector = "H1" }
counts = 2
)toml";

collect::RunData baseline_rule_data() {
  collect::RunData d;
  d.series[skey("Ar40", "H1", collect::kSignal, 0, 0)] = collect::Series{{10, 11, 12}, {5, 5, 5}, {}, {}};
  return d;
}

// A baseline position whose `at` is the signal position's target wins over
// one nearer in time.
TEST(FitResults, BaselineRuleAt) {
  const auto plan = plan_from(kBaselineRulePlan);
  auto d = baseline_rule_data();
  d.series[skey("Ar40", "H1", collect::kBaseline, 0, 1)] = collect::Series{{20, 21}, {1, 1}, {}, {}};
  d.series[skey("", "H1", collect::kBaseline, 2, 0)] = collect::Series{{13, 14}, {3, 3}, {}, {}};
  const auto out = fit_results(d, plan);
  ASSERT_EQ(out.results.baseline_positions.size(), 2u);
  const auto& c = out.results.baseline_choices.at("Ar40@H1");
  EXPECT_EQ(c.rule, "at");
  EXPECT_DOUBLE_EQ(c.value, 1);
  ASSERT_EQ(c.baselines.size(), 1u);
  EXPECT_EQ(out.results.baseline_positions[c.baselines[0]].segment, 0);
  EXPECT_EQ(out.results.baseline_positions[c.baselines[0]].position, 1);
  EXPECT_EQ(out.results.baseline_positions[c.baselines[0]].when, "after_inlet");
  EXPECT_DOUBLE_EQ(out.results.baselines.at("H1").value, 1);
}

// No `at` matches and nothing brackets: the nearest by mean time.
TEST(FitResults, BaselineRuleNearest) {
  const auto plan = plan_from(kBaselineRulePlan);
  auto d = baseline_rule_data();
  d.series[skey("", "H1", collect::kBaseline, 1, 0)] = collect::Series{{30, 31}, {7, 7}, {}, {}};
  d.series[skey("", "H1", collect::kBaseline, 2, 0)] = collect::Series{{13, 14}, {3, 3}, {}, {}};
  const auto out = fit_results(d, plan);
  const auto& c = out.results.baseline_choices.at("Ar40@H1");
  EXPECT_EQ(c.rule, "nearest");
  EXPECT_DOUBLE_EQ(c.value, 3);
  EXPECT_EQ(out.results.baseline_positions[c.baselines.at(0)].when, "after_main");
}

// One before the signal, one after, neither matching: their average, the
// error a mean of two.
TEST(FitResults, BaselineRuleBracketingAverage) {
  const auto plan = plan_from(kBaselineRulePlan);
  auto d = baseline_rule_data();
  d.series[skey("", "H1", collect::kBaseline, 1, 0)] = collect::Series{{0, 1}, {1, 2}, {}, {}};
  d.series[skey("", "H1", collect::kBaseline, 2, 0)] = collect::Series{{20, 21}, {3, 5}, {}, {}};
  const auto out = fit_results(d, plan);
  ASSERT_EQ(out.results.baseline_positions.size(), 2u);
  const auto& b1 = out.results.baseline_positions[0].result;
  const auto& b2 = out.results.baseline_positions[1].result;
  EXPECT_DOUBLE_EQ(b1.value, 1.5);
  EXPECT_DOUBLE_EQ(b2.value, 4);
  EXPECT_GT(b1.error, 0);
  const auto& c = out.results.baseline_choices.at("Ar40@H1");
  EXPECT_EQ(c.rule, "bracketing_average");
  EXPECT_EQ(c.baselines, (std::vector<std::size_t>{0, 1}));
  EXPECT_DOUBLE_EQ(c.value, 2.75);
  EXPECT_DOUBLE_EQ(c.error, std::sqrt(b1.error * b1.error + b2.error * b2.error) / 2);
  EXPECT_DOUBLE_EQ(out.results.baselines.at("H1").value, 2.75);
}

TEST_F(EngineTest, IsotopeOnTwoDetectorsGetsDetectorQualifiedIntercepts) {
  collect::RunData d;
  d.series[skey("Ar36", "CDD", collect::kSignal)] = collect::Series{{1, 2, 3}, {1, 2, 3}, {}, {}};
  d.series[skey("Ar36", "L2", collect::kSignal)] = collect::Series{{1, 2, 3}, {2, 4, 6}, {}, {}};
  d.series[skey("Ar40", "H1", collect::kSignal)] = collect::Series{{1}, {5}, {}, {}};  // too few for linear
  d.series[skey("Ar39", "AX", collect::kSignal)] =
      collect::Series{{1, 2}, {1, std::numeric_limits<double>::quiet_NaN()}, {}, {}};
  auto out = fit_results(d, plan::MeasurementPlan{});
  EXPECT_TRUE(out.results.intercepts.contains("Ar36@CDD"));
  EXPECT_TRUE(out.results.intercepts.contains("Ar36@L2"));
  // One point: averaged, and the record says which fit was used.
  ASSERT_TRUE(out.results.intercepts.contains("Ar40@H1"));
  EXPECT_EQ(out.results.intercepts.at("Ar40@H1").fit.kind, reduction::FitKind::Average);
  EXPECT_DOUBLE_EQ(out.results.intercepts.at("Ar40@H1").intercept.value, 5);
  // Non-finite data cannot be fitted.
  EXPECT_FALSE(out.results.intercepts.contains("Ar39@AX"));
  ASSERT_EQ(out.errors.size(), 1u);
  EXPECT_EQ(out.errors[0].substr(0, 8), "Ar39@AX:");
}

}  // namespace

// ---- a silence in the readings (executor hardening, spec 4.4) ----------------

class MeasurementGaps : public EngineTest {
 protected:
  MeasurementGaps() {
    progress_ = bus_.subscribe<CountsProgress>([this](const CountsProgress& e) {
      if (e.block != Block::Main) return;
      if (e.i == 1) first_main_ = clock_.now();
      if (silence_after_ && e.i == *silence_after_ && !spec_.silent_from) {
        spec_.silent_from = clock_.now();
        if (silence_for_) spec_.silent_until = clock_.now() + *silence_for_;
        valves_at_silence_ = valves_.log.size();
        spec_log_at_silence_ = spec_.log.size();
      }
      if (e.i == (silence_after_ ? *silence_after_ : 0) + 1 && spec_.silent_from) {
        valves_at_resume_ = valves_.log.size();
        spec_log_at_resume_ = spec_.log.size();
      }
      last_main_ = clock_.now();
    });
    gaps_ = bus_.subscribe<measurement::MeasurementGap>([this](const measurement::MeasurementGap& g) { published_.push_back(g); });
    alarms_ = bus_.subscribe<Alarm>([this](const Alarm& a) { said_.push_back(a); });
  }

  // Silent after the n-th main reading, for `d` (for good when not given).
  void silence(int n, std::optional<pychron::Duration> d) {
    silence_after_ = n;
    silence_for_ = d;
  }
  // When the main collection began: one integration (1 s) before its first reading.
  TimePoint main_began() const { return *first_main_ - 1s; }

  std::optional<int> silence_after_;
  std::optional<pychron::Duration> silence_for_;
  std::optional<TimePoint> first_main_, last_main_;
  std::size_t valves_at_silence_ = 0, valves_at_resume_ = 0, spec_log_at_silence_ = 0, spec_log_at_resume_ = 0;
  std::vector<measurement::MeasurementGap> published_;
  std::vector<Alarm> said_;
  SignalBus::Subscription progress_, gaps_, alarms_;
};

TEST_F(MeasurementGaps, AShortSilenceIsRecordedAndTheBlockEndsOnTime) {
  auto p = multicollect(100);
  silence(30, 20s);
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_FALSE(r.truncated_by_link);
  ASSERT_EQ(r.gaps.size(), 1u);
  EXPECT_EQ(r.gaps[0].block, Block::Main);
  // From the last reading before the silence to the first after it.
  EXPECT_NEAR(r.gaps[0].to - r.gaps[0].from, 21.0, 1e-9);
  ASSERT_EQ(published_.size(), 1u);
  EXPECT_EQ(published_[0].from, r.gaps[0].from);
  // The block ends at its planned end (100 s after it began), with what 80 s gives.
  EXPECT_EQ(*last_main_ - main_began(), 100s);
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 80);
}

TEST_F(MeasurementGaps, NothingIsDoneToTheLineWhileWaiting) {
  auto p = multicollect(100);
  silence(30, 20s);
  const auto r = run(p);
  ASSERT_EQ(r.gaps.size(), 1u);
  EXPECT_EQ(valves_at_resume_, valves_at_silence_);
  EXPECT_EQ(spec_log_at_resume_, spec_log_at_silence_);  // no protect, no position, no restart
}

TEST_F(MeasurementGaps, ASilenceLongerThanMaxGapEndsTheBlock) {
  auto p = multicollect(300);
  silence(100, std::nullopt);
  const auto r = run(p);
  ASSERT_EQ(r.gaps.size(), 1u);
  // Waited max_gap (120 s) from the last reading, then the acquisition was
  // started again once, which gave nothing within a reading's timeout (8 s).
  EXPECT_EQ(*last_main_ - main_began(), 100s);
  const auto restarts = std::count(spec_.log.begin() + static_cast<std::ptrdiff_t>(spec_log_at_silence_), spec_.log.end(),
                                   "start 1.000000");
  EXPECT_EQ(restarts, 1);
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 100);
}

TEST_F(MeasurementGaps, EnoughCountsIsTruncatedWithReasonLink) {
  auto p = multicollect(300);
  silence(100, std::nullopt);  // 100 of 300: above 20 %
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Truncated) << (r.error ? r.error->what : "");
  EXPECT_TRUE(r.truncated_by_link);
  EXPECT_FALSE(spec_.acquiring);
  EXPECT_FALSE(valves_.state["B"]);
}

TEST_F(MeasurementGaps, TooFewCountsIsFailed) {
  auto p = multicollect(300);
  silence(30, std::nullopt);  // 30 of 300: under 20 %
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->what.find("30 of 300"), std::string::npos) << r.error->what;
}

// 20 % of 5 is one count; a linear fit needs two: the fit's is the floor.
TEST_F(MeasurementGaps, TheFitsMinimumIsTheFloor) {
  auto p = multicollect(5);
  silence(1, std::nullopt);
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->what.find("1 of 5"), std::string::npos) << r.error->what;
}

TEST_F(MeasurementGaps, ALinkThatFailsAtOnceIsWaitedOutOnTheClock) {
  auto p = multicollect(100);
  spec_.silence_is_link_down = true;
  spec_.link_down_costs = 1ms;  // a clock that moves between two reads of it, as a real one
  silence(30, 20s);
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_EQ(r.gaps.size(), 1u);
  EXPECT_GE(*last_main_ - main_began(), 100s);  // each failing call cost 1 ms
  EXPECT_LT(*last_main_ - main_began(), 101s);
  EXPECT_LT(spec_.silent_calls, 40);  // waited, not spun
}

TEST_F(MeasurementGaps, AnAcquisitionStartOfUnknownOutcomeIsRestartedOnce) {
  auto p = multicollect(10);
  spec_.lose_starts = 1;
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_GE(spec_.log.size(), 3u);
  const auto lost = std::find(spec_.log.begin(), spec_.log.end(), "start lost");
  ASSERT_NE(lost, spec_.log.end());
  EXPECT_EQ(*(lost + 1), "stop");
  EXPECT_EQ(*(lost + 2), "start 1.000000");
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 10);
}

TEST_F(MeasurementGaps, EquilibrationFollowsTheSameRule) {
  auto p = multicollect(10);
  // Silent from the third sniff reading for 15 s, longer than a reading's
  // timeout (8 s): a gap, waited out without moving the inlet, which still
  // closes on time.
  spec_.on_reading = [this](int n) {
    if (n == 3) {
      spec_.silent_from = clock_.now();
      spec_.silent_until = clock_.now() + 15s;
    }
  };
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_EQ(r.gaps.size(), 1u);
  EXPECT_EQ(r.gaps[0].block, Block::Equilibrate);
  EXPECT_FALSE(r.gaps[0].resumed);  // silent until the inlet closed
  const auto& tm = r.data.timing;
  ASSERT_TRUE(tm.inlet_open && tm.inlet_close);
  EXPECT_NEAR(*tm.inlet_close - *tm.inlet_open, 10.0, 1e-9);  // as without a gap
  EXPECT_FALSE(valves_.state["B"]);
}

TEST_F(MeasurementGaps, WithNoGapNothingChanges) {
  const auto r = run(multicollect(10));
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed);
  EXPECT_TRUE(r.gaps.empty());
  EXPECT_FALSE(r.truncated_by_link);
  EXPECT_TRUE(published_.empty());
  EXPECT_TRUE(said_.empty());
}

// Spec 4.4, valve row: in a measurement the inlet close and a detector's
// unprotect keep trying for the critical window, with an alarm at 20 s.
TEST_F(MeasurementGaps, AValveCallInMeasurementKeepsTryingForTheCriticalWindow) {
  std::optional<TimePoint> first_try, closed;
  valves_.close_fault = [&](const std::string& v) -> Result<void> {
    if (v != "B") return {};
    if (!first_try) first_try = clock_.now();
    if (clock_.now() - *first_try < 5min) return fail(ErrorKind::Io, "valve bus down");
    closed = clock_.now();
    return {};
  };
  const auto r = run(multicollect(10));
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_TRUE(closed);
  EXPECT_GE(*closed - *first_try, 5min);
  EXPECT_LE(*closed - *first_try, 5min + 30s);
  ASSERT_EQ(said_.size(), 1u);
  EXPECT_EQ(said_[0].severity, AlarmSeverity::Warning);
  EXPECT_EQ(said_[0].ts - *first_try, 20s);
}

TEST_F(MeasurementGaps, AValveDownForGoodFailsTheMeasurementAfterTheCriticalWindow) {
  std::optional<TimePoint> first_try;
  valves_.close_fault = [&](const std::string& v) -> Result<void> {
    if (v != "B") return {};
    if (!first_try) first_try = clock_.now();
    return fail(ErrorKind::Io, "valve bus down");
  };
  const auto r = run(multicollect(10));
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  ASSERT_EQ(said_.size(), 2u);
  EXPECT_EQ(said_[1].severity, AlarmSeverity::Critical);
  EXPECT_EQ(said_[1].ts - *first_try, 10min);
}

TEST_F(MeasurementGaps, ADetectorUnprotectKeepsTryingForTheCriticalWindow) {
  auto p = multicollect(10);
  p.main.hops.front().protect = {"CDD"};
  std::optional<TimePoint> first_try, done;
  spec_.protect_fault = [&](const std::string& det, bool on) -> Result<void> {
    if (on || det != "CDD") return {};
    if (!first_try) first_try = clock_.now();
    if (clock_.now() - *first_try < 5min) return fail(ErrorKind::Io, "spectrometer link down");
    done = clock_.now();
    return {};
  };
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  ASSERT_TRUE(done);
  EXPECT_GE(*done - *first_try, 5min);
  ASSERT_FALSE(said_.empty());
  EXPECT_EQ(said_[0].severity, AlarmSeverity::Warning);
}

// A conditional over a window sees only readings after the gap in a real run.
TEST_F(MeasurementGaps, AConditionalsWindowStartsAgainAfterTheGap) {
  auto p = multicollect(100);
  silence(30, 20s);
  spec_.ts_lag = 500ms;  // a reading is stamped when it was integrated, before it arrives
  MetricRef ar40;
  ar40.a = "Ar40";
  std::optional<std::size_t> from_at_resume;
  auto sub = bus_.subscribe<CountsProgress>([&](const CountsProgress& e) {
    if (e.block == Block::Main && e.i == 31 && engine_ != nullptr)
      from_at_resume = engine_->collector().metrics().contiguous_from(ar40);
  });
  const auto r = run(p);
  ASSERT_EQ(r.gaps.size(), 1u);
  ASSERT_TRUE(from_at_resume);
  EXPECT_EQ(*from_at_resume, 30u);  // the 31st point is the first after the gap
}

// Review of the branch: what a gap must not do.

TEST_F(MeasurementGaps, ACancelAfterAGapIsACancelNotAFailure) {
  auto p = multicollect(300);
  silence(30, 20s);
  auto sub = bus_.subscribe<CountsProgress>([this](const CountsProgress& e) {
    if (e.block == Block::Main && e.i == 35) token_.cancel();
  });
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Cancelled) << (r.error ? r.error->what : "");
}

TEST_F(MeasurementGaps, StartingAgainDoesNotRunPastThePlannedEnd) {
  auto p = multicollect(124);
  silence(1, std::nullopt);  // max_gap ends at 121 s; the planned end is 124 s
  std::optional<TimePoint> main_done;
  auto sub = bus_.subscribe<BlockFinished>([&](const BlockFinished& e) {
    if (e.block == Block::Main) main_done = clock_.now();
  });
  const auto r = run(p);
  ASSERT_TRUE(main_done);
  EXPECT_LE(*main_done - main_began(), 124s);
  EXPECT_EQ(r.gaps.size(), 1u);
}

TEST_F(MeasurementGaps, ADefiniteErrorInAGapFailsAndKeepsTheGap) {
  auto p = multicollect(100);
  spec_.silence_ends_in_error = true;
  silence(30, 20s);
  const auto r = run(p);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_EQ(r.error->kind, ErrorKind::Protocol);
  ASSERT_EQ(r.gaps.size(), 1u);
  EXPECT_FALSE(r.gaps[0].resumed);
}

TEST_F(MeasurementGaps, AStartLostTwiceLeavesTheAcquisitionStopped) {
  auto p = multicollect(10);
  spec_.lose_starts = 2;  // the sniff's start, then its one retry
  (void)run(p);
  const auto second = std::find(std::find(spec_.log.begin(), spec_.log.end(), "start lost") + 1, spec_.log.end(),
                                "start lost");
  ASSERT_NE(second, spec_.log.end());
  ASSERT_NE(second + 1, spec_.log.end());
  EXPECT_EQ(*(second + 1), "stop");  // it may have started: stopped, to be sure
}

// ---- the engine runs segments (measurement plan v2, Task 6) -------------------

namespace {

// The extraction-line aliases the plan fixtures name.
class FixtureLine final : public plan::IAliasResolver {
 public:
  std::optional<ParamValue> resolve_alias(std::string_view key) const override {
    if (key == "valves.inlet") return std::string("B");
    if (key == "valves.outlet") return std::string("C");
    if (key == "extraction.eqtime") return std::int64_t{20};
    return std::nullopt;
  }
};

plan::MeasurementPlan load_fixture_plan(const std::string& name) {
  std::ifstream in(std::filesystem::path(PYCHRON_PLAN_FIXTURES_DIR) / "valid" / name);
  std::stringstream text;
  text << in.rdbuf();
  auto t = plan::parse_plan_template(text.str(), name);
  EXPECT_TRUE(t.has_value()) << (t ? "" : t.error().what);
  if (!t) return {};
  const FixtureLine line;
  auto loaded = plan::load_plan(*t, {}, plan::PlanResolvers{&line, nullptr});
  EXPECT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().what);
  return loaded ? loaded->plan : plan::MeasurementPlan{};
}

}  // namespace

class EngineV2 : public EngineTest {
 protected:
  struct Run {
    MeasurementResult result;
    TimePoint end{};
  };

  Run run_fixture(const std::string& name) {
    clock_.set(t0_);
    spec_.log.clear();
    valves_.log.clear();
    valves_.state.clear();
    peak_.requests.clear();
    hook_.entries.clear();
    Run out;
    out.result = run(load_fixture_plan(name));
    out.end = clock_.now();
    return out;
  }

  // The same plan written as v1 tables and as v2 segments runs the same.
  void expect_same_run(const std::string& v1, const std::string& v2) {
    const auto a = run_fixture(v1);
    const auto b = run_fixture(v2);
    ASSERT_EQ(a.result.outcome, MeasurementOutcome::Completed) << (a.result.error ? a.result.error->what : "");
    ASSERT_EQ(b.result.outcome, MeasurementOutcome::Completed) << (b.result.error ? b.result.error->what : "");
    EXPECT_FALSE(a.result.trace.empty());
    EXPECT_EQ(a.result.trace, b.result.trace);
    EXPECT_EQ(a.result.blocks, b.result.blocks);
    EXPECT_EQ(a.end, b.end);
    ASSERT_EQ(a.result.data.series.size(), b.result.data.series.size());
    for (const auto& [key, s] : a.result.data.series) {
      ASSERT_TRUE(b.result.data.series.contains(key)) << to_string(key);
      EXPECT_EQ(s.t, b.result.data.series.at(key).t) << to_string(key);
      EXPECT_EQ(s.v, b.result.data.series.at(key).v) << to_string(key);
    }
    EXPECT_EQ(a.result.data.counts, b.result.data.counts);
  }
};

TEST_F(EngineV2, V1AndV2SpellingTraceEqual) {
  expect_same_run("multicollect.toml", "v2_spelling_of_multicollect.toml");
}

TEST_F(EngineV2, V1AndV2SpellingTraceEqualForPeakHops) {
  expect_same_run("peak_hop_cdd.toml", "v2_spelling_of_peak_hop_cdd.toml");
}

// The trace says what was done, in order, and when.
TEST_F(EngineV2, TheTraceRecordsMovesSettlesProtectionAndCollections) {
  auto p = multicollect(3);
  p.sniff.enabled = false;
  p.main.hops.front().protect = {"CDD"};
  const auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  std::vector<std::string> what;
  what.reserve(r.trace.size());
  for (const auto& e : r.trace) what.push_back(e.what + " " + e.detail);
  EXPECT_EQ(what, (std::vector<std::string>{"protect CDD on", "move Ar40@H1", "settle 2", "protect CDD off",
                                            "collect 3"}));
  ASSERT_EQ(r.trace.size(), 5u);
  EXPECT_EQ(r.trace[1].segment, 0);  // the main segment, positioned before equilibration
  EXPECT_EQ(r.trace[1].position, 0);
  EXPECT_DOUBLE_EQ(r.trace[1].t, 0);
  EXPECT_DOUBLE_EQ(r.trace[4].t, 14);  // after the 2 s settle and the 12 s equilibration
}

// A whiff reads where the first position put the magnet: no move of its own.
TEST_F(EngineV2, AWhiffReadsWhereTheMagnetIs) {
  auto p = multicollect(3);
  p.main.hops.front().position = TestHopPosition{"Ar39", "AX"};
  p.whiff = {true, 2, 1, {{"Ar40.cur > 1e9", "abort"}}};
  const auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_EQ(std::count_if(spec_.log.begin(), spec_.log.end(), [](const std::string& l) { return l.starts_with("position"); }),
            1);
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kWhiff), 2);
}

// An excluded cup the hop protects is still protected during the move.
TEST_F(EngineV2, AnExcludedCupTheHopProtectsIsStillProtected) {
  auto p = multicollect(3);
  p.sniff.enabled = false;
  p.detectors.exclude = {"CDD"};
  p.main.hops.front().protect = {"CDD"};
  const auto r = run(p);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  EXPECT_NE(std::find(spec_.log.begin(), spec_.log.end(), "protect CDD"), spec_.log.end());
  EXPECT_TRUE(spec_.protected_.empty());
  EXPECT_FALSE(merged_series(r.data, "Ar36", "CDD", collect::kSignal).has_value());
}

// ---- position execution (measurement plan v2 spec 5.2, Task 8) ---------------

namespace {

constexpr std::string_view kPlanHeader = R"(
[plan]
name = "positions"
instrument_family = "fake"
)";

std::vector<std::string> what_of(const std::vector<TraceEntry>& trace) {
  std::vector<std::string> out;
  out.reserve(trace.size());
  for (const auto& e : trace) out.push_back(e.what + " " + e.detail);
  return out;
}

// The entries of one segment's position, in order.
std::vector<std::string> at_position(const std::vector<TraceEntry>& trace, int segment, int position) {
  std::vector<TraceEntry> picked;
  std::copy_if(trace.begin(), trace.end(), std::back_inserter(picked),
               [&](const TraceEntry& e) { return e.segment == segment && e.position == position; });
  return what_of(picked);
}

}  // namespace

class PositionExec : public EngineV2 {
 protected:
  MeasurementResult run_toml(std::string_view body) {
    const std::string toml = std::string(kPlanHeader) + std::string(body);
    auto r = run(plan_from(toml));
    EXPECT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
    return r;
  }
};

// Worked plan 7.2 (with a reference named, so the first two positions have a
// magnet target): CDD is deflected at the first position only, and put back
// before the second, which does not set it.
TEST_F(PositionExec, WorkedPlan72DeflectsCddOnlyAtTheFirstPosition) {
  spec_.detectors = {"H1", "H2", "L1", "L2", "CDD"};
  const auto r = run_toml(R"(
[detectors]
reference = "H1"

[[segment]]
kind = "signal"
cycles = 20
integration_s = 1
[[segment.positions]]
collect = [
  { label = "Ar40", detector = "H1" }, { label = "Ar41", detector = "H2" },
  { label = "Ar38", detector = "L1" }, { label = "Ar37", detector = "L2" },
  { label = "Ar36", detector = "CDD", deflection = 110 } ]
counts = 15
settle_s = 6
[[segment.positions]]
collect = { Ar39 = "CDD" }
at = { label = "Ar39", detector = "CDD" }
counts = 15
settle_s = 5
[[segment.positions]]
collect = { mass35 = "CDD" }
at = { mass = 35.0, detector = "CDD" }
counts = 8
settle_s = 3

[[segment]]
kind = "baseline"
when = "after_main"
[[segment.positions]]
collect = { Ar40 = "H1", Ar41 = "H2", Ar38 = "L1", Ar37 = "L2", Ar36 = "CDD" }
at = { mass = 34.2, detector = "H1" }
counts = 120
settle_s = 20
)");
  const auto what = what_of(r.trace);
  ASSERT_GE(what.size(), 22u);
  const std::vector<std::string> cycle1{"deflect CDD=110", "move Ar40@H1",      "settle 6", "collect 15",
                                        "deflect CDD=0",   "move Ar39@CDD",     "settle 5", "collect 15",
                                        "move mass 35@CDD", "settle 3",         "collect 8"};
  EXPECT_EQ(std::vector<std::string>(what.begin(), what.begin() + 11), cycle1);
  // Cycle 2 begins as cycle 1 did: CDD is deflected again before the move.
  EXPECT_EQ(std::vector<std::string>(what.begin() + 11, what.begin() + 15),
            (std::vector<std::string>{"deflect CDD=110", "move Ar40@H1", "settle 6", "collect 15"}));
  EXPECT_EQ(std::count(what.begin(), what.end(), "deflect CDD=110"), 20);
  EXPECT_EQ(std::count(what.begin(), what.end(), "deflect CDD=0"), 20);
  // The baseline after main sees CDD as it was.
  EXPECT_EQ(std::vector<std::string>(what.end() - 3, what.end()),
            (std::vector<std::string>{"move mass 34.2@H1", "settle 20", "collect 120"}));
  EXPECT_DOUBLE_EQ(spec_.deflections.at("CDD"), 0);
  EXPECT_EQ(count(r, "mass35", "CDD", collect::kSignal), 20 * 8);
}

// Worked plan 7.4, second position: the inactive H2(CDD) is protected for the
// move and deflected, and nothing is collected on it.
TEST_F(PositionExec, InactiveEntryIsProtectedNotCollected) {
  spec_.detectors = {"H2", "H2(CDD)", "H1(CDD)", "AX(CDD)", "L1(CDD)", "L2(CDD)"};
  const auto r = run_fixture("v2_74_helix_hydride_hcl.toml").result;
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
  const auto pos2 = at_position(r.trace, 0, 1);
  ASSERT_GE(pos2.size(), 11u);
  EXPECT_EQ(std::vector<std::string>(pos2.begin(), pos2.begin() + 11),
            (std::vector<std::string>{"protect H2(CDD) on", "deflect H2(CDD)=3250", "deflect H1(CDD)=200",
                                      "deflect L1(CDD)=600", "move Ar38@AX(CDD)", "settle 3", "protect H2(CDD) off",
                                      "collect 10", "deflect H1(CDD)=0", "deflect H2(CDD)=0",
                                      "deflect L1(CDD)=0"}));
  EXPECT_FALSE(merged_series(r.data, "Ar40", "H2(CDD)", collect::kSignal).has_value());
  EXPECT_TRUE(merged_series(r.data, "Ar40", "H2", collect::kSignal).has_value());
  EXPECT_TRUE(merged_series(r.data, "Ar39", "H1(CDD)", collect::kSignal).has_value());
  EXPECT_TRUE(spec_.protected_.empty());
}

// Worked plan 7.5 without its optics: acquisition restarts with each
// position's integration, and the trace says so where it changes.
TEST_F(PositionExec, IntegrationChangeRestartsAcquisition) {
  spec_.detectors = {"F0", "IC0", "IC1", "IC2"};
  const auto r = run_toml(R"(
[[segment]]
kind = "signal"
cycles = 15
[[segment.positions]]
collect = { Ar40 = "F0", Ar38 = "IC1", Ar36 = "IC2" }
counts = 10
settle_s = 5
integration_s = 1
[[segment.positions]]
collect = { Ar39 = "IC0", Ar37 = "IC1" }
counts = 10
settle_s = 5
integration_s = 2
)");
  std::vector<std::string> changes;
  for (const auto& e : r.trace)
    if (e.what == "integration") changes.push_back(std::to_string(e.position) + ":" + e.detail);
  // The first collection has nothing to differ from; after it, every
  // position changes the integration, every cycle.
  std::vector<std::string> expected{"1:2"};
  for (int cycle = 1; cycle < 15; ++cycle) {
    expected.emplace_back("0:1");
    expected.emplace_back("1:2");
  }
  EXPECT_EQ(changes, expected);
  EXPECT_EQ(std::count(spec_.log.begin(), spec_.log.end(), "start 2.000000"), 15);
  EXPECT_EQ(std::count(spec_.log.begin(), spec_.log.end(), "start 1.000000"), 15);
  const auto t = merged_series(r.data, "Ar39", "IC0", collect::kSignal).value().t;
  ASSERT_GE(t.size(), 2u);
  EXPECT_DOUBLE_EQ(t[1] - t[0], 2);  // readings 2 s apart
}

// One position, three cycles: one move and one settle; its deflection is set
// once and put back when the segment ends.
TEST_F(PositionExec, UnchangedPositionSkipsMoveAndSettle) {
  const auto r = run_toml(R"(
[detectors]
reference = "H1"
[[segment]]
cycles = 3
[[segment.positions]]
collect = [ { label = "Ar40", detector = "H1" }, { label = "Ar36", detector = "CDD", deflection = 50 } ]
counts = 4
settle_s = 2
)");
  EXPECT_EQ(what_of(r.trace),
            (std::vector<std::string>{"deflect CDD=50", "move Ar40@H1", "settle 2", "collect 4", "collect 4",
                                      "collect 4", "deflect CDD=0"}));
  EXPECT_EQ(count(r, "Ar40", "H1", collect::kSignal), 12);
}

TEST_F(PositionExec, DacAndHvTargets) {
  auto r = run_toml(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { dac = 1.234 }
counts = 2
)");
  EXPECT_NE(std::find(spec_.log.begin(), spec_.log.end(), "move_native 1.234"), spec_.log.end());
  EXPECT_EQ(what_of(r.trace).front(), "move dac 1.234");
  spec_.log.clear();
  r = run_toml(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H1" }
at = { hv = 4500, detector = "H1" }
counts = 2
)");
  EXPECT_NE(std::find(spec_.log.begin(), spec_.log.end(), "position_hv 4500@H1"), spec_.log.end());
  EXPECT_EQ(what_of(r.trace).front(), "move hv 4500@H1");
}

// A position's table, else its segment's, is selected before the move; going
// to another table moves the magnet again.
TEST_F(PositionExec, TheTableIsSelectedPerPosition) {
  const auto r = run_toml(R"(
[detectors]
reference = "H1"
[[segment]]
cycles = 2
table = "t1"
[[segment.positions]]
collect = { Ar40 = "H1" }
counts = 1
settle_s = 1
[[segment.positions]]
collect = { Ar40 = "H1" }
table = "t2"
counts = 1
settle_s = 1
)");
  EXPECT_EQ(what_of(r.trace),
            (std::vector<std::string>{"table t1", "move Ar40@H1", "settle 1", "collect 1",  //
                                      "table t2", "move Ar40@H1", "settle 1", "collect 1",  //
                                      "table t1", "move Ar40@H1", "settle 1", "collect 1",  //
                                      "table t2", "move Ar40@H1", "settle 1", "collect 1"}));
  EXPECT_EQ(std::count(spec_.log.begin(), spec_.log.end(), "table t2"), 2);
}

// Large-beam settle and logical detector modes (measurement plan v2 spec
// 3.6 and 5.2): H2 has threshold 5 and settles 20 s after a large beam;
// H2(CDD) is H2 in CDD mode, 2 s to switch.
class LargeBeamSettle : public PositionExec {
 protected:
  LargeBeamSettle() {
    spec_.detectors = {"H1", "H2", "H2(CDD)"};
    // Ar40 on H2 reads about 110 (10 x the fake's growth), far over 5; at
    // Ar39@H1, H2 sees Ar38 at about 0.11.
    spec_.layout = {{"Ar40@H2", {{"H2", "Ar40"}}},
                    {"Ar39@H1", {{"H1", "Ar39"}, {"H2", "Ar38"}}},
                    {"Ar40@H2(CDD)", {{"H2(CDD)", "Ar40"}}}};
    spec_.abundance = {{"Ar40", 10}, {"Ar39", 1}, {"Ar38", 0.01}};
    timing_["H2"] = DetectorTiming{5.0, 20, "", 0};
    timing_["H2(CDD)"] = DetectorTiming{std::nullopt, 0, "H2", 2};
  }

  static std::vector<std::string> settles(const MeasurementResult& r) {
    std::vector<std::string> out;
    for (const auto& e : r.trace)
      if (e.what == "settle") out.push_back(std::to_string(e.segment) + ":" + std::to_string(e.position) + " " + e.detail);
    return out;
  }
};

TEST_F(LargeBeamSettle, WaitsTheConfiguredSettle) {
  const auto r = run_toml(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H2" }
at = { label = "Ar40", detector = "H2" }
counts = 2
settle_s = 1
[[segment.positions]]
collect = { Ar39 = "H1", Ar38 = "H2" }
at = { label = "Ar39", detector = "H1" }
counts = 2
settle_s = 3
)");
  EXPECT_EQ(settles(r), (std::vector<std::string>{"0:0 1", "0:1 20"}));
}

// A detector without the large beam, or not in the next position, does not wait.
TEST_F(LargeBeamSettle, OnlyADetectorThatSawALargeBeamWaits) {
  spec_.abundance["Ar40"] = 0.01;  // about 0.11 on H2: under 5
  const auto r = run_toml(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H2" }
at = { label = "Ar40", detector = "H2" }
counts = 2
settle_s = 1
[[segment.positions]]
collect = { Ar39 = "H1", Ar38 = "H2" }
at = { label = "Ar39", detector = "H1" }
counts = 2
settle_s = 3
)");
  EXPECT_EQ(settles(r), (std::vector<std::string>{"0:0 1", "0:1 3"}));
}

TEST_F(LargeBeamSettle, BaselineDoesNotClearIt) {
  const auto r = run_toml(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H2" }
at = { label = "Ar40", detector = "H2" }
counts = 2
settle_s = 1
[[segment.positions]]
kind = "baseline"
collect = { Ar40 = "H2" }
at = { mass = 34.2, detector = "H2" }
counts = 2
settle_s = 3
[[segment.positions]]
collect = { Ar39 = "H1", Ar38 = "H2" }
at = { label = "Ar39", detector = "H1" }
counts = 2
settle_s = 3
)");
  EXPECT_EQ(settles(r), (std::vector<std::string>{"0:0 1", "0:1 20", "0:2 20"}));
}

// Review focus 4: the state is the measurement's, not the segment's.
TEST_F(LargeBeamSettle, StateCarriesAcrossSegments) {
  const auto r = run_toml(R"(
[[segment]]
[[segment.positions]]
collect = { Ar40 = "H2" }
at = { label = "Ar40", detector = "H2" }
counts = 2
settle_s = 1

[[segment]]
[[segment.positions]]
collect = { Ar39 = "H1", Ar38 = "H2" }
at = { label = "Ar39", detector = "H1" }
counts = 2
settle_s = 3
)");
  EXPECT_EQ(settles(r), (std::vector<std::string>{"0:0 1", "1:0 20"}));
}

TEST_F(LargeBeamSettle, ModeSwitchWaits) {
  timing_["H2"].large_beam_threshold.reset();  // only the mode switch here
  const auto r = run_toml(R"toml(
[[segment]]
cycles = 2
[[segment.positions]]
collect = { Ar40 = "H2" }
at = { label = "Ar40", detector = "H2" }
counts = 1
settle_s = 1
[[segment.positions]]
collect = { Ar40 = "H2(CDD)" }
at = { label = "Ar40", detector = "H2(CDD)" }
counts = 1
settle_s = 1
)toml");
  // The first position has no mode to switch from; every switch after it,
  // into CDD and back, waits mode_switch_s.
  EXPECT_EQ(settles(r), (std::vector<std::string>{"0:0 1", "0:1 2", "0:0 2", "0:1 2"}));
}
