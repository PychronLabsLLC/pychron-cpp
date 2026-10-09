// Run state machine and phases (experiment spec 3.1) against fakes.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "pychron/experiment/record/serialize.hpp"
#include "run_fakes.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::run;
using namespace pychron::experiment::fakes;
using AutomatedRun = pychron::experiment::run::Run;

namespace {

TEST(RunStateMachine, LegalAndIllegalTransitions) {
  using S = RunState;
  using E = RunEvent;
  const std::vector<std::tuple<S, E, S>> legal = {
      {S::Pending, E::Start, S::Preparing},         {S::Preparing, E::Prepared, S::Extracting},
      {S::Extracting, E::Extracted, S::Equilibrating}, {S::Equilibrating, E::Equilibrated, S::Measuring},
      {S::Measuring, E::Truncate, S::Truncated},    {S::Measuring, E::Measured, S::PostMeasuring},
      {S::Truncated, E::Measured, S::PostMeasuring}, {S::Equilibrating, E::Measured, S::PostMeasuring},
      {S::PostMeasuring, E::PostMeasured, S::Saving}, {S::Saving, E::Saved, S::Success},
  };
  const S all_states[] = {S::Pending,   S::Preparing,     S::Extracting, S::Equilibrating, S::Measuring, S::Truncated,
                          S::PostMeasuring, S::Saving,   S::Success,    S::Failed,        S::Cancelled, S::Aborted};
  const E all_events[] = {E::Start,    E::Prepared,     E::Extracted, E::Equilibrated, E::Truncate, E::Measured,
                          E::PostMeasured, E::Saved, E::Fail,      E::Cancel,       E::Abort};
  for (auto s : all_states) {
    for (auto e : all_events) {
      std::optional<S> expected;
      for (const auto& [from, ev, to] : legal)
        if (from == s && ev == e) expected = to;
      if (!is_terminal(s)) {
        if (e == E::Fail) expected = S::Failed;
        if (e == E::Cancel) expected = S::Cancelled;
        if (e == E::Abort) expected = S::Aborted;
      }
      EXPECT_EQ(transition(s, e), expected) << to_string(s) << " --" << to_string(e) << "-->";
    }
  }
}

TEST(RunStateMachine, PublishesAndRejects) {
  ManualClock clock;
  SignalBus bus;
  std::vector<RunStateChanged> seen;
  auto sub = bus.subscribe<RunStateChanged>([&](const RunStateChanged& e) { seen.push_back(e); });
  RunStateMachine sm("r1", clock, &bus);
  ASSERT_TRUE(sm.advance(RunEvent::Start, "go"));
  auto bad = sm.advance(RunEvent::Saved);
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().what.find("illegal transition saved from preparing"), std::string::npos);
  EXPECT_EQ(sm.state(), RunState::Preparing);
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].run_id, "r1");
  EXPECT_EQ(seen[0].reason, "go");
  ASSERT_TRUE(sm.advance(RunEvent::Cancel));
  EXPECT_FALSE(sm.advance(RunEvent::Fail));  // terminal
  EXPECT_EQ(sm.history().size(), 2u);
}

class RunTest : public ::testing::Test {
 protected:
  RunServices services() {
    RunServices s;
    s.clock = &clock_;
    s.bus = &bus_;
    s.scripts = &host_;
    s.resolver = &resolver_;
    s.line.device = &device_;
    s.line.cryo = cryo_;
    s.spectrometer = &spec_;
    s.valves = &valves_;
    s.spectrometer_info = [] { return SpectrometerInfo{"hash123", "argon-2026", 1.0}; };
    s.engine.sleep = [this](pychron::Duration d) { clock_.advance(d); };
    s.plans = &plans_;
    s.aliquots = &lab_.aliquots;
    s.persister = &lab_.files;
    s.save = &lab_.save;
    s.instrument.mass_spectrometer = "argus";
    return s;
  }

  RunResult go(RunSpec spec, RunHooks hooks = {}) {
    AutomatedRun run(std::move(spec), queue_, services(), std::move(hooks), 3, 7);
    return run.execute(control_);
  }

  std::vector<RunState> states(const RunResult& r) {
    std::vector<RunState> out{RunState::Pending};
    for (const auto& h : r.history) out.push_back(h.to);
    return out;
  }

  ManualClock clock_{TimePoint{} + 1000s};
  SignalBus bus_;
  FakeScriptHost host_;
  AnyScriptResolver resolver_;
  FakeDevice device_;
  FakeSpectrometer spec_{clock_};
  FakeValves valves_;
  plan::PlanLibrary plans_ = test_plans();
  Lab lab_;
  QueueSpec queue_ = [] {
    QueueSpec q;
    q.name = "q1";
    q.mass_spectrometer = "argus";
    return q;
  }();
  RunControl control_;
  extraction::ICryo* cryo_ = nullptr;
};

// A line cryostat with two inputs, or one whose read fails.
class FakeLineCryo final : public extraction::ICryo {
 public:
  Result<void> set_cryo(double) override { return {}; }
  Result<double> get_cryo_temp(int) override { return 77.3; }
  Result<std::map<std::string, double>> read_cryo_inputs() override {
    if (broken) return fail(ErrorKind::Timeout, "no reply within 500 ms");
    return std::map<std::string, double>{{"A", 77.3}, {"B", 80.1}};
  }
  bool broken = false;
};

TEST_F(RunTest, TheCryostatsTemperaturesAreRecordedAsExtractionEnds) {
  // Owner decision 2026-10-05: measured beside requested.
  FakeLineCryo cryo;
  cryo_ = &cryo;
  auto spec = unknown_run("12345");
  spec.extraction.cryo_temp = 77.0;
  auto r = go(spec);
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  ASSERT_TRUE(r.record);
  EXPECT_EQ(r.record->extraction.spec.cryo_temperature, 77.0);
  EXPECT_EQ(r.record->extraction.actuals.cryo_measured, (std::map<std::string, double>{{"A", 77.3}, {"B", 80.1}}));
}

TEST_F(RunTest, ACryostatThatDoesNotAnswerIsSaidAndLeftOut) {
  FakeLineCryo cryo;
  cryo.broken = true;
  cryo_ = &cryo;
  auto r = go(unknown_run("12345"));
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  ASSERT_TRUE(r.record);
  EXPECT_TRUE(r.record->extraction.actuals.cryo_measured.empty());
  bool said = false;
  for (const auto& m : r.messages) said |= m.find("cryo temperatures not recorded") != std::string::npos;
  EXPECT_TRUE(said) << ::testing::PrintToString(r.messages);
}

TEST_F(RunTest, FullRunSucceedsAndSaves) {
  int overlap = 0, pump = 0;
  RunHooks hooks;
  hooks.on_overlap_ready = [&] { ++overlap; };
  hooks.on_pump_time_started = [&] { ++pump; };
  auto r = go(unknown_run("12345"), hooks);
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "") << ::testing::PrintToString(r.messages);
  using S = RunState;
  EXPECT_EQ(states(r), (std::vector<S>{S::Pending, S::Preparing, S::Extracting, S::Equilibrating, S::Measuring,
                                       S::PostMeasuring, S::Saving, S::Success}));
  EXPECT_EQ(host_.ran_scripts(), (std::vector<std::string>{"extract", "post_eq", "post_meas"}));
  EXPECT_GE(device_.end_extracts.load(), 1);
  EXPECT_GE(device_.disables.load(), 1);
  EXPECT_EQ(overlap, 1);
  EXPECT_EQ(pump, 1);
  EXPECT_EQ(r.aliquot, 1);
  ASSERT_TRUE(r.record);
  EXPECT_EQ(r.record->identity.identifier, "12345");
  EXPECT_EQ(r.record->identity.run_index, 3);
  EXPECT_EQ(r.record->instrument.mass_spectrometer, "argus");
  EXPECT_EQ(r.record->measurement.scripts.at("extraction").sha, "sha-extract");
  EXPECT_TRUE(r.record->results.intercepts.contains("Ar40"));
  EXPECT_TRUE(r.record->results.baselines.contains("H1"));
  EXPECT_FALSE(r.record->provenance.sha.empty());
  EXPECT_FALSE(r.record->events.empty());
  EXPECT_TRUE(std::filesystem::exists(lab_.files.analysis_path(*r.record)));
  EXPECT_TRUE(std::filesystem::exists(lab_.dir() / "records" / "12345" / "12345-1.extraction.json"));
  EXPECT_EQ(lab_.save.pending(), 0u);
  // The next run of the identifier gets the next aliquot.
  RunControl c2;
  AutomatedRun second(unknown_run("12345"), queue_, services());
  EXPECT_EQ(second.execute(c2).aliquot, 2);
}

TEST_F(RunTest, TruncateSavesAsTruncatedSuccess) {
  spec_.on_reading = [&](int n) {
    if (n == 8) control_.truncate();
  };
  auto r = go(unknown_run("12345"));
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  EXPECT_TRUE(r.truncated);
  const auto st = states(r);
  EXPECT_NE(std::find(st.begin(), st.end(), RunState::Truncated), st.end());
}

// The run's own state does not depend on anyone listening: it is measuring
// while the main block runs, bus or no bus.
TEST_F(RunTest, TheRunIsMeasuringDuringTheMainBlockWithoutABus) {
  auto s = services();
  s.bus = nullptr;
  AutomatedRun run(unknown_run("12345"), queue_, std::move(s), {}, 3, 7);
  std::optional<RunState> during;
  spec_.on_reading = [&](int n) {
    if (n == 8) during = run.state();
  };
  auto r = run.execute(control_);
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  ASSERT_TRUE(during);
  EXPECT_EQ(*during, RunState::Measuring);
}

TEST_F(RunTest, TruncateBeforeMeasurementApplies) {
  control_.truncate(true);  // e.g. requested during extraction
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Success);
  EXPECT_TRUE(r.truncated);
  EXPECT_DOUBLE_EQ(r.measurement.count_scale, 0.25);
}

TEST_F(RunTest, CancelDuringExtractionRunsPostMeasurementAndSavesNothing) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken& token) -> Result<void> {
    token.cancel();
    return fail(ErrorKind::Cancelled, "ScriptCancelled");
  };
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Cancelled);
  EXPECT_EQ(host_.ran_scripts(), (std::vector<std::string>{"extract", "post_meas"}));
  EXPECT_GE(device_.disables.load(), 1);
  EXPECT_FALSE(r.record);
  EXPECT_FALSE(std::filesystem::exists(lab_.dir() / "records" / "12345" / "12345-1.json"));
}

TEST_F(RunTest, AbortDuringMeasurementStopsWithoutPostMeasurement) {
  spec_.on_reading = [&](int n) {
    if (n == 3) control_.abort();
  };
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Aborted);
  for (const auto& s : host_.ran_scripts()) EXPECT_NE(s, "post_meas");
  EXPECT_FALSE(r.record);
  EXPECT_FALSE(valves_.is_open("B"));  // the engine closed the inlet
}

TEST_F(RunTest, CancelDuringMeasurementStillRunsPostMeasurement) {
  spec_.on_reading = [&](int n) {
    if (n == 3) control_.cancel();
  };
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Cancelled);
  const auto ran = host_.ran_scripts();
  EXPECT_EQ(ran.back(), "post_meas");
  EXPECT_FALSE(r.record);
}

TEST_F(RunTest, PostMeasurementFailureStillSaves) {
  host_.bodies["post_meas"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "pump valve stuck");
  };
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Success);
  ASSERT_TRUE(r.record);
  bool noted = false;
  for (const auto& m : r.messages) noted |= m.find("pump valve stuck") != std::string::npos;
  EXPECT_TRUE(noted);
}

// What a run says (a script's info(), a hole move's note) is published as it
// is said and kept in the run's record.
TEST_F(RunTest, WhatARunSaysIsPublishedAndKeptInItsRecord) {
  host_.bodies["extract"] = [this](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    clock_.advance(5s);
    env.log("hole 3: centered, moved 0.150, -0.100 mm (residual 0.010 mm)");
    return {};
  };
  host_.bodies["post_meas"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "pump valve stuck");
  };
  std::vector<RunNote> said;
  auto sub = bus_.subscribe<RunNote>([&](const RunNote& e) { said.push_back(e); });
  const TimePoint started = clock_.now();
  auto r = go(unknown_run("12345"));
  ASSERT_EQ(r.state, RunState::Success);

  ASSERT_GE(said.size(), 2u);
  EXPECT_EQ(said[0].run_id, r.uuid);
  EXPECT_EQ(said[0].row, 7u);  // the queue row the run was made for
  EXPECT_EQ(said[0].text, "hole 3: centered, moved 0.150, -0.100 mm (residual 0.010 mm)");
  EXPECT_EQ(said[0].ts, started + 5s);
  EXPECT_NE(said[1].text.find("pump valve stuck"), std::string::npos);
  std::vector<std::string> texts;
  for (const auto& e : said) texts.push_back(e.text);
  EXPECT_EQ(texts, r.messages);

  ASSERT_TRUE(r.record);
  std::vector<record::Event> notes;
  for (const auto& e : r.record->events)
    if (e.kind == "note") notes.push_back(e);
  ASSERT_EQ(notes.size(), r.messages.size());
  EXPECT_EQ(notes[0].detail, said[0].text);
  EXPECT_DOUBLE_EQ(notes[0].t, 5.0);  // seconds since the run started, as the state events
  EXPECT_NE(notes[1].detail.find("pump valve stuck"), std::string::npos);
  // and the saved file has them
  std::ifstream in(lab_.files.analysis_path(*r.record));
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto saved = record::from_json(text);
  ASSERT_TRUE(saved) << saved.error().what;
  EXPECT_EQ(saved->events, r.record->events);
}

TEST_F(RunTest, IncompleteRecordFailsWithSaveErrorAndStaysInTheSpool) {
  auto s = services();
  s.spectrometer_info = nullptr;  // no state hash: the record cannot be finalized
  AutomatedRun run(unknown_run("12345"), queue_, s);
  auto r = run.execute(control_);
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_TRUE(r.save_error);
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->what.find("spectrometer.state_hash"), std::string::npos);
  ASSERT_TRUE(r.record);
  EXPECT_TRUE(r.record->provenance.sha.empty());  // a draft
  // The pipeline still handed it to the persister; nothing measured is lost.
  EXPECT_TRUE(std::filesystem::exists(lab_.files.analysis_path(*r.record)));
}

TEST_F(RunTest, PrepareFailures) {
  auto r = go(unknown_run("12345", "no_such_plan"));
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_TRUE(host_.ran_scripts().empty());

  resolver_.missing = {"extract"};
  r = go(unknown_run("12346"));
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_NE(r.error->what.find("no script 'extract'"), std::string::npos);

  resolver_.missing.clear();
  auto s = services();
  s.scripts = nullptr;
  AutomatedRun run(unknown_run("12347"), queue_, s);
  RunControl c;
  auto r2 = run.execute(c);
  EXPECT_EQ(r2.state, RunState::Failed);
  EXPECT_NE(r2.error->what.find("no script host"), std::string::npos);
}

TEST_F(RunTest, DegasRunsExtractionOnly) {
  RunSpec degas = unknown_run("dg");
  degas.id.type = AnalysisType::Degas;
  degas.measurement.plan.clear();
  degas.post_equilibration.reset();
  auto r = go(degas);
  EXPECT_EQ(r.state, RunState::Success);
  EXPECT_EQ(spec_.readings.load(), 0);
  EXPECT_FALSE(r.record);
  EXPECT_EQ(host_.ran_scripts(), (std::vector<std::string>{"extract", "post_meas"}));
}

TEST_F(RunTest, ResourceHooksWrapPhasesAndCanCancel) {
  std::vector<std::string> calls;
  RunHooks hooks;
  hooks.acquire_extraction = [&] {
    calls.emplace_back("+extraction");
    return true;
  };
  hooks.release_extraction = [&] { calls.emplace_back("-extraction"); };
  hooks.acquire_spectrometer = [&] {
    calls.emplace_back("+spectrometer");
    return true;
  };
  hooks.release_spectrometer = [&] { calls.emplace_back("-spectrometer"); };
  auto r = go(unknown_run("12345"), hooks);
  EXPECT_EQ(r.state, RunState::Success);
  EXPECT_EQ(calls, (std::vector<std::string>{"+extraction", "-extraction", "+spectrometer", "-spectrometer"}));

  // Cancelled while waiting for the spectrometer: nothing is measured.
  const int readings = spec_.readings.load();
  RunControl c;
  hooks.acquire_spectrometer = [&c] {
    c.cancel();
    return false;
  };
  AutomatedRun waiting(unknown_run("12346"), queue_, services(), hooks);
  auto r2 = waiting.execute(c);
  EXPECT_EQ(r2.state, RunState::Cancelled);
  EXPECT_EQ(spec_.readings.load(), readings);
}

// --- the run's extraction device (laser system design, section 5) -----------

class RunDeviceTest : public RunTest {
 protected:
  // No device set directly: the run asks for its own by name.
  RunServices by_name() {
    RunServices s = services();
    s.line.device = nullptr;
    s.devices = [this](std::string_view name) -> extraction::IExtractionDevice* {
      asked_.emplace_back(name);
      if (name == "co2") return &co2_;
      if (name == "diode") return &diode_;
      return nullptr;
    };
    // NOLINTNEXTLINE(clang-analyzer-core.StackAddressEscape): returned by value; the function captures `this` only
    return s;
  }
  void extract_on_the_bound_device() {
    host_.bodies["extract"] = [this](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
      if (env.line.device == nullptr) return fail(ErrorKind::Config, "no extraction device");
      trays_at_script_ = co2_.fake_stage.trays();
      if (auto r = env.line.device->enable(); !r) return r;
      return env.line.device->extract(5, extraction::ExtractUnits::Percent);
    };
  }

  FakeDevice co2_{"co2", true};
  FakeDevice diode_{"diode", true};
  std::vector<std::string> asked_;
  std::vector<std::string> trays_at_script_;
};

TEST_F(RunDeviceTest, BindsTheDeviceItsSpecNames) {
  queue_.extract_device = "co2";
  extract_on_the_bound_device();
  auto spec = unknown_run("12345");
  spec.extraction.device = "diode";
  AutomatedRun run(std::move(spec), queue_, by_name());
  const auto r = run.execute(control_);
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  EXPECT_EQ(diode_.extracts.load(), 1);
  EXPECT_EQ(co2_.extracts.load(), 0);
  EXPECT_EQ(asked_, (std::vector<std::string>{"diode"}));  // resolved once
}

TEST_F(RunDeviceTest, TheQueuesDeviceIsTheDefault) {
  queue_.extract_device = "co2";
  extract_on_the_bound_device();
  AutomatedRun run(unknown_run("12345"), queue_, by_name());
  ASSERT_EQ(run.execute(control_).state, RunState::Success);
  EXPECT_EQ(co2_.extracts.load(), 1);
  EXPECT_EQ(diode_.extracts.load(), 0);
}

TEST_F(RunDeviceTest, SetsTheQueuesTrayBeforeTheScript) {
  queue_.extract_device = "co2";
  queue_.tray = "small";
  extract_on_the_bound_device();
  AutomatedRun run(unknown_run("12345"), queue_, by_name());
  ASSERT_EQ(run.execute(control_).state, RunState::Success);
  EXPECT_EQ(trays_at_script_, (std::vector<std::string>{"small"}));
  EXPECT_TRUE(diode_.fake_stage.trays().empty());
}

// The device outlives the queue: a tray left set by the last queue must not
// give this one's hole names a meaning.
TEST_F(RunDeviceTest, WithNoTrayTheStagesTrayIsCleared) {
  queue_.extract_device = "co2";
  AutomatedRun run(unknown_run("12345"), queue_, by_name());
  ASSERT_EQ(run.execute(control_).state, RunState::Success);
  EXPECT_EQ(co2_.fake_stage.trays(), (std::vector<std::string>{""}));
}

TEST_F(RunDeviceTest, EndsOnlyItsOwnDevice) {
  queue_.extract_device = "co2";
  extract_on_the_bound_device();
  auto spec = unknown_run("12345");
  spec.extraction.device = "diode";
  AutomatedRun run(std::move(spec), queue_, by_name());
  ASSERT_EQ(run.execute(control_).state, RunState::Success);
  EXPECT_GE(diode_.end_extracts.load(), 1);
  EXPECT_GE(diode_.disables.load(), 1);
  EXPECT_FALSE(diode_.enabled.load());
  EXPECT_EQ(co2_.end_extracts.load(), 0);
  EXPECT_EQ(co2_.disables.load(), 0);
}

TEST_F(RunDeviceTest, AnUnknownTrayFailsTheRunBeforeAnyScript) {
  queue_.extract_device = "co2";
  queue_.tray = "no-such-tray";
  AutomatedRun run(unknown_run("12345"), queue_, by_name());
  const auto r = run.execute(control_);
  EXPECT_EQ(r.state, RunState::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->what.find("no-such-tray"), std::string::npos);
  EXPECT_TRUE(host_.ran_scripts().empty());
  EXPECT_EQ(co2_.extracts.load(), 0);
}

TEST_F(RunDeviceTest, ADeviceSetDirectlyWins) {
  queue_.extract_device = "co2";
  extract_on_the_bound_device();
  RunServices s = by_name();
  s.line.device = &device_;
  AutomatedRun run(unknown_run("12345"), queue_, s);
  ASSERT_EQ(run.execute(control_).state, RunState::Success);
  EXPECT_EQ(co2_.extracts.load(), 0);
  EXPECT_GE(device_.end_extracts.load(), 1);
  EXPECT_TRUE(asked_.empty());
}

// The run's pattern reaches its script, which is what execute_pattern() runs.
TEST_F(RunDeviceTest, TheScriptIsToldTheRunsPattern) {
  queue_.extract_device = "co2";
  std::string seen = "unset";
  host_.bodies["extract"] = [&](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    const auto it = env.context.globals.find("pattern");
    if (it != env.context.globals.end() && std::holds_alternative<std::string>(it->second)) {
      seen = std::get<std::string>(it->second);
    }
    return {};
  };
  auto spec = unknown_run("12345");
  spec.extraction.pattern = "hexagon";
  AutomatedRun with(std::move(spec), queue_, by_name());
  ASSERT_EQ(with.execute(control_).state, RunState::Success);
  EXPECT_EQ(seen, "hexagon");

  seen = "unset";
  AutomatedRun without(unknown_run("12346"), queue_, by_name());
  ASSERT_EQ(without.execute(control_).state, RunState::Success);
  EXPECT_EQ(seen, "");  // no pattern: an empty name, so a script can test it
}

// A pattern never outlives the run that started it: whatever the script left
// going is stopped before the laser is switched off, so the next run starts
// from a device that is doing nothing.
TEST_F(RunDeviceTest, TheRunStopsAPatternItsScriptLeftRunning) {
  queue_.extract_device = "co2";
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    return env.line.device->pattern_runner()->execute_pattern("hexagon");  // and never waits for it
  };
  AutomatedRun run(unknown_run("12345"), queue_, by_name());
  ASSERT_EQ(run.execute(control_).state, RunState::Success);
  EXPECT_EQ(co2_.fake_patterns.stops.load(), 1);
  EXPECT_FALSE(*co2_.fake_patterns.running());
  EXPECT_FALSE(co2_.pattern_running_at_end.load());  // stopped first, then the laser off
  EXPECT_EQ(diode_.fake_patterns.stops.load(), 0);
}

TEST_F(RunDeviceTest, AnUnknownNameLeavesTheRunWithoutADevice) {
  queue_.extract_device = "furnace";
  bool had_device = true;
  host_.bodies["extract"] = [&](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    had_device = env.line.device != nullptr;
    return {};
  };
  AutomatedRun run(unknown_run("12345"), queue_, by_name());
  ASSERT_EQ(run.execute(control_).state, RunState::Success);
  EXPECT_FALSE(had_device);
}

TEST_F(RunDeviceTest, NoResolverAndNoDeviceIsAsBefore) {
  queue_.extract_device = "co2";
  queue_.tray = "small";
  RunServices s = services();
  s.line.device = nullptr;
  AutomatedRun run(unknown_run("12345"), queue_, s);
  EXPECT_EQ(run.execute(control_).state, RunState::Success);
}

}  // namespace
