// Run state machine and phases (experiment spec 3.1) against fakes.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "pychron/experiment/record/serialize.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "run_fakes.hpp"
#include "virtual_time.hpp"

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

TEST(RunState, FromStringIsToStringsInverse) {
  for (int i = static_cast<int>(RunState::Pending); i <= static_cast<int>(RunState::Aborted); ++i) {
    const auto s = static_cast<RunState>(i);
    EXPECT_EQ(run_state_from(to_string(s)), s) << to_string(s);
  }
  EXPECT_EQ(run_state_from("nope"), std::nullopt);
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
  EXPECT_TRUE(r.record->results.intercepts.contains("Ar40@H1"));
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

// The persister of a pipeline that will not take the record. Everything else
// is the lab's files.
class RefusingPersister final : public persist::IAnalysisPersister {
 public:
  explicit RefusingPersister(persist::FilePersister& files) : files_(files) {}
  Result<int> next_aliquot(const std::string& identifier) override { return files_.next_aliquot(identifier); }
  Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override { return files_.begin_run(id, queue); }
  Result<void> save_extraction(const record::AnalysisRecord& r) override { return files_.save_extraction(r); }
  Result<void> save_analysis(const record::AnalysisRecord&) override {
    return fail(ErrorKind::Io, "identifier '12345' is unknown");
  }
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override {
    return files_.save_artifact(uuid, name, bytes);
  }
  Result<void> flush() override { return {}; }
  Result<std::vector<std::string>> check_queue(const QueueSpec&) override { return std::vector<std::string>{}; }

 private:
  persist::FilePersister& files_;
};

// The run was measured and its record is safe in the spool: it is a success,
// and it is flagged, and it says why it is not where it should be.
TEST_F(RunTest, ARunThePersisterDidNotTakeSucceedsFlaggedAndSaysWhy) {
  RefusingPersister refusing(lab_.files);
  persist::SavePipeline pipeline(lab_.spool, refusing);
  auto s = services();
  s.persister = &refusing;
  s.save = &pipeline;
  std::vector<RunNote> said;
  auto sub = bus_.subscribe<RunNote>([&](const RunNote& n) { said.push_back(n); });
  AutomatedRun run(unknown_run("12345"), queue_, s);
  auto r = run.execute(control_);
  EXPECT_EQ(r.state, RunState::Success);
  EXPECT_TRUE(r.save_error);
  EXPECT_FALSE(r.error.has_value());
  ASSERT_FALSE(r.messages.empty());
  EXPECT_EQ(r.messages.back(), "not saved to the data store, kept in the spool: identifier '12345' is unknown");
  ASSERT_FALSE(said.empty());
  EXPECT_EQ(said.back().text, r.messages.back());
  EXPECT_EQ(pipeline.pending(), 1u);
  ASSERT_TRUE(r.record);
  EXPECT_FALSE(r.record->provenance.sha.empty());  // finalized: it can be sent as it is
  ASSERT_TRUE(lab_.spool.get(r.record->identity.uuid));
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

// A plan that measures during the extraction (spec 5.4) claims the
// spectrometer before the extraction line and lets it go last.
TEST_F(RunTest, DuringExtractionClaimsTheSpectrometerAtExtraction) {
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
  auto r = go(unknown_run("12345", "syn"), hooks);
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "") << ::testing::PrintToString(r.messages);
  EXPECT_EQ(calls, (std::vector<std::string>{"+spectrometer", "+extraction", "-extraction", "-spectrometer"}));
  ASSERT_TRUE(r.record);
  int during = 0;
  for (const auto& b : r.record->results.baseline_positions) during += b.when == "during_extraction" ? 1 : 0;
  EXPECT_EQ(during, 2);  // H1 and AX
  // Its readings came before the inlet opened: before time zero.
  for (const auto& s : r.record->data.series) {
    if (s.kind == "baseline") {
      EXPECT_LT(s.trace.t.back(), 0.f);
    }
  }
}

// A failed extraction stops the phase and lets the spectrometer go; the run
// fails as an extraction failure does.
TEST_F(RunTest, DuringExtractionAFailedExtractionReleasesTheSpectrometer) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "laser link down");
  };
  std::vector<std::string> calls;
  RunHooks hooks;
  hooks.acquire_spectrometer = [&] {
    calls.emplace_back("+spectrometer");
    return true;
  };
  hooks.release_spectrometer = [&] { calls.emplace_back("-spectrometer"); };
  auto r = go(unknown_run("12345", "syn"), hooks);
  EXPECT_EQ(r.state, RunState::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_EQ(r.error->what, "laser link down");
  EXPECT_EQ(calls, (std::vector<std::string>{"+spectrometer", "-spectrometer"}));
  EXPECT_FALSE(spec_.acquiring.load());
  ASSERT_TRUE(r.record);
  EXPECT_EQ(r.record->provenance.status, record::RecordStatus::Failed);
}

// A run with no extraction script has no time for such a phase.
TEST_F(RunTest, DuringExtractionNeedsAnExtractionScript) {
  auto spec = unknown_run("12345", "syn");
  spec.extraction.script.clear();
  auto r = go(spec);
  EXPECT_EQ(r.state, RunState::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->what.find("during_extraction"), std::string::npos) << r.error->what;
}

// --- a staged release's prepare scripts (spec 11, plan Task 26) ---------------

TEST_F(RunTest, PrepareScriptsRunWithPlanVariables) {
  auto spec = unknown_run("12345", "staged");
  spec.extraction.cryo_temp = 20.0;
  auto r = go(spec);
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "") << ::testing::PrintToString(r.messages);
  const auto ran = host_.ran_scripts();
  const auto globals = host_.globals();
  std::vector<std::size_t> prepares;
  for (std::size_t i = 0; i < ran.size(); ++i)
    if (ran[i] == "cryo:release") prepares.push_back(i);
  ASSERT_EQ(prepares.size(), 2u) << ::testing::PrintToString(ran);
  EXPECT_EQ(globals[prepares[0]].at("cryo_temp"), scripting::Value{std::int64_t{35}});
  EXPECT_EQ(globals[prepares[1]].at("cryo_temp"), scripting::Value{std::int64_t{75}});
  EXPECT_EQ(globals[prepares[0]].at("hold_s"), scripting::Value{std::int64_t{300}});
  // The run's own variables are there under them.
  EXPECT_EQ(globals[prepares[0]].at("cryo_temperature"), scripting::Value{20.0});
  // After the extraction script, before anything after the measurement.
  EXPECT_EQ(ran.front(), "extract");
  // Recorded a gas each.
  ASSERT_TRUE(r.record);
  ASSERT_EQ(r.record->data.gases.size(), 2u);
  const auto& he = r.record->data.gases[0];
  ASSERT_TRUE(he.prepare);
  EXPECT_EQ(he.prepare->script, "cryo:release");
  EXPECT_EQ(he.prepare->variables.at("cryo_temp"), "35");
  EXPECT_FALSE(he.prepare->started.empty());
  EXPECT_LE(he.prepare->started, he.prepare->finished);
  ASSERT_TRUE(he.pump_out);
  EXPECT_EQ(he.pump_out->valve, "IP");
  EXPECT_EQ(he.pump_out->time_s, 60);
  EXPECT_EQ(r.record->data.gases[1].prepare->variables.at("cryo_temp"), "75");
  EXPECT_FALSE(r.record->data.gases[1].pump_out);
}

TEST_F(RunTest, PrepareScriptRunsAsExtractionKind) {
  auto r = go(unknown_run("12345", "staged"));
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  const auto ran = host_.ran_scripts();
  const auto kinds = host_.kinds();
  int n = 0;
  for (std::size_t i = 0; i < ran.size(); ++i) {
    if (ran[i] != "cryo:release") continue;
    ++n;
    EXPECT_EQ(kinds[i], scripting::ScriptKind::Extraction);
  }
  EXPECT_EQ(n, 2);
}

TEST_F(RunTest, StagedPlanHoldsTheExtractionLineForTheWholeRun) {
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
  auto r = go(unknown_run("12345", "staged"), hooks);
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  EXPECT_EQ(calls, (std::vector<std::string>{"+extraction", "+spectrometer", "-spectrometer", "-extraction"}));
}

// Spec 11.2: a failed prepare ends the run as a failed extraction does;
// what the gases before it measured is saved.
TEST_F(RunTest, FailingPrepareSavesEarlierGases) {
  host_.bodies["cryo:release"] = [](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    if (env.context.globals.at("cryo_temp") == scripting::Value{std::int64_t{75}})
      return fail(ErrorKind::Io, "cryostat did not reach 75 K");
    return {};
  };
  auto r = go(unknown_run("12345", "staged"));
  EXPECT_EQ(r.state, RunState::Failed);
  ASSERT_TRUE(r.record);
  EXPECT_EQ(r.record->provenance.status, record::RecordStatus::Failed);
  EXPECT_NE(r.record->provenance.failure.find("gas 'Ne'"), std::string::npos) << r.record->provenance.failure;
  EXPECT_NE(r.record->provenance.failure.find("cryostat did not reach 75 K"), std::string::npos);
  EXPECT_TRUE(r.record->results.intercepts.contains("He4@H1"));
  EXPECT_FALSE(r.record->results.intercepts.contains("Ne20@AX"));
  EXPECT_TRUE(std::filesystem::exists(lab_.files.analysis_path(*r.record)));
}

// What the host's static check refuses fails the gas with the host's words.
TEST_F(RunTest, PrepareScriptRefusedByTheHostFailsTheGas) {
  host_.bodies["cryo:release"] = [](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    if (env.context.globals.at("cryo_temp") == scripting::Value{std::int64_t{75}})
      return fail(ErrorKind::Config, "cryo:release:3: 'position' is not defined");
    return {};
  };
  auto r = go(unknown_run("12345", "staged"));
  EXPECT_EQ(r.state, RunState::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->what.find("gas 'Ne'"), std::string::npos) << r.error->what;
  EXPECT_NE(r.error->what.find("'position' is not defined"), std::string::npos) << r.error->what;
  ASSERT_TRUE(r.record);
  EXPECT_TRUE(r.record->results.intercepts.contains("He4@H1"));
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

// ---- a run that fails after extraction began (executor hardening, spec 4.5) --

namespace {

// The finished records of `identifier` on disk, read back.
std::vector<record::AnalysisRecord> saved_records(const std::filesystem::path& lab, const std::string& identifier) {
  std::vector<record::AnalysisRecord> out;
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(lab / "records" / identifier, ec)) {
    const auto name = f.path().filename().string();
    if (f.path().extension() != ".json" || name.ends_with(".extraction.json")) continue;
    std::ifstream in(f.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (auto r = record::from_json(text)) out.push_back(*r);
  }
  return out;
}

}  // namespace

// Nothing of the sample is spent before extraction, and nothing is saved.
// (The aliquot number was issued already: FilePersister does not give it
// again in this process, as before this spec.)
TEST_F(RunTest, AFailureInPrepareSavesNothing) {
  auto r = go(unknown_run("12345", "no_such_plan"));
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_FALSE(r.record);
  EXPECT_TRUE(saved_records(lab_.dir(), "12345").empty());
  EXPECT_TRUE(host_.ran_scripts().empty());
}

TEST_F(RunTest, AFailureInExtractionIsSavedAsFailed) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "laser link down");
  };
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Failed);
  ASSERT_TRUE(r.error);
  EXPECT_EQ(r.error->what, "laser link down");  // the run's error, not the save's
  EXPECT_FALSE(r.save_error);
  ASSERT_TRUE(r.record);
  EXPECT_EQ(r.record->provenance.status, record::RecordStatus::Failed);
  EXPECT_EQ(r.record->provenance.failure, "laser link down");
  EXPECT_FALSE(r.record->provenance.sha.empty());  // finalized: the store takes it
  EXPECT_TRUE(r.record->data.series.empty());
  EXPECT_GE(r.record->extraction.actuals.duration, 0.0);
  EXPECT_FALSE(r.record->events.empty());
  const auto saved = saved_records(lab_.dir(), "12345");
  ASSERT_EQ(saved.size(), 1u);
  EXPECT_EQ(saved[0], *r.record);
  EXPECT_EQ(lab_.save.pending(), 0u);
}

TEST_F(RunTest, AFailureInMeasurementKeepsTheSeriesCollected) {
  spec_.fail_from = 6;
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Failed);
  ASSERT_TRUE(r.record);
  EXPECT_EQ(r.record->provenance.status, record::RecordStatus::Failed);
  EXPECT_NE(r.record->provenance.failure.find("spectrometer link down"), std::string::npos)
      << r.record->provenance.failure;
  ASSERT_FALSE(r.record->data.series.empty());
  std::size_t points = 0;
  for (const auto& s : r.record->data.series) points += s.trace.t.size();
  EXPECT_GT(points, 0u);
  EXPECT_EQ(saved_records(lab_.dir(), "12345").size(), 1u);
}

TEST_F(RunTest, ATruncatedRunIsMarkedTruncated) {
  spec_.on_reading = [&](int n) {
    if (n == 8) control_.truncate();
  };
  auto r = go(unknown_run("12345"));
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  ASSERT_TRUE(r.record);
  EXPECT_EQ(r.record->provenance.status, record::RecordStatus::Truncated);
  EXPECT_TRUE(r.record->provenance.failure.empty());
  const auto saved = saved_records(lab_.dir(), "12345");
  ASSERT_EQ(saved.size(), 1u);
  EXPECT_EQ(saved[0].provenance.status, record::RecordStatus::Truncated);
}

TEST_F(RunTest, ACompleteRunIsMarkedComplete) {
  auto r = go(unknown_run("12345"));
  ASSERT_EQ(r.state, RunState::Success) << (r.error ? r.error->what : "");
  ASSERT_TRUE(r.record);
  EXPECT_EQ(r.record->provenance.status, record::RecordStatus::Complete);
}

// The operator's choice, not a link failure: nothing is saved (spec 4.4).
TEST_F(RunTest, ACancelledOrAbortedRunIsStillNotSaved) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken& token) -> Result<void> {
    token.cancel();
    return fail(ErrorKind::Cancelled, "ScriptCancelled");
  };
  auto cancelled = go(unknown_run("12345"));
  EXPECT_EQ(cancelled.state, RunState::Cancelled);
  EXPECT_FALSE(cancelled.record);

  host_.bodies.erase("extract");
  RunControl c;
  spec_.on_reading = [&](int n) {
    if (n == 4) c.token().abort();
  };
  AutomatedRun run(unknown_run("12346"), queue_, services());
  auto aborted = run.execute(c);
  EXPECT_EQ(aborted.state, RunState::Aborted);
  EXPECT_FALSE(aborted.record);
  EXPECT_TRUE(saved_records(lab_.dir(), "12345").empty());
  EXPECT_TRUE(saved_records(lab_.dir(), "12346").empty());
}

// Spec section 5: the record is written before the run is over, so before
// the executor publishes RunFinished, which comes after execute() returns.
TEST_F(RunTest, TheRecordReachesTheSpoolBeforeRunFinished) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "laser link down");
  };
  std::optional<std::size_t> on_disk_when_failed;
  auto sub = bus_.subscribe<RunStateChanged>([&](const RunStateChanged& e) {
    if (e.to == RunState::Failed) on_disk_when_failed = saved_records(lab_.dir(), "12345").size();
  });
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Failed);
  ASSERT_TRUE(on_disk_when_failed);
  EXPECT_EQ(*on_disk_when_failed, 1u);
}

TEST_F(RunTest, AFailedRunsAliquotIsNotGivenAgain) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "laser link down");
  };
  auto failed = go(unknown_run("12345"));
  EXPECT_EQ(failed.state, RunState::Failed);
  EXPECT_EQ(failed.aliquot, 1);
  host_.bodies.erase("extract");
  RunControl c;
  AutomatedRun next(unknown_run("12345"), queue_, services());
  EXPECT_EQ(next.execute(c).aliquot, 2);
}

// The point of no return is the extraction script's first line (spec 4.4): a
// run without one spent no sample, and a failure saves nothing, as before.
TEST_F(RunTest, ARunWithoutAnExtractionScriptThatFailsSavesNothing) {
  auto spec = unknown_run("12345");
  spec.extraction.script.clear();
  spec_.fail_from = 6;
  auto r = go(spec);
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_FALSE(r.past_no_return);
  EXPECT_FALSE(r.record);
  EXPECT_TRUE(saved_records(lab_.dir(), "12345").empty());
}

// ---- safe state (executor hardening, spec 4.7) -------------------------------

TEST_F(RunTest, AFailedRunPastNoReturnIsMadeSafe) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "laser link down");
  };
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_FALSE(r.unsafe);
  EXPECT_TRUE(r.unsafe_steps.empty());
  ASSERT_FALSE(valves_.log.empty());
  EXPECT_EQ(valves_.log.back(), "close B");  // the plan's inlet
  EXPECT_GE(device_.end_extracts.load(), 2);  // at the end of extraction, and again
}

TEST_F(RunTest, AFailedPrepareIsNot) {
  auto r = go(unknown_run("12345", "no_such_plan"));
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_TRUE(valves_.log.empty());
  EXPECT_EQ(device_.end_extracts.load(), 0);
}

// Spec 4.4, PostMeasure row: as before, a note and the run still saves; and
// the line is made safe.
TEST_F(RunTest, APostMeasureFailureStillSavesAndIsMadeSafe) {
  host_.bodies["post_meas"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "pump valve link down");
  };
  auto r = go(unknown_run("12345"));
  EXPECT_EQ(r.state, RunState::Success);
  ASSERT_TRUE(r.record);
  EXPECT_EQ(saved_records(lab_.dir(), "12345").size(), 1u);
  EXPECT_FALSE(r.unsafe);
  ASSERT_FALSE(valves_.log.empty());
  EXPECT_EQ(valves_.log.back(), "close B");
}

TEST_F(RunTest, ASuccessfulRunIsNotMadeSafeAgain) {
  auto r = go(unknown_run("12345"));
  ASSERT_EQ(r.state, RunState::Success);
  EXPECT_EQ(device_.end_extracts.load(), 1);
}

namespace {

const TimePoint kSafeStart = TimePoint{} + std::chrono::hours(1);

VirtualClock::Options safe_clock() {
  VirtualClock::Options o;
  o.start = kSafeStart;
  return o;
}

}  // namespace

class SafeState : public pychron::testing::VirtualTimeTest {
 protected:
  SafeState() {
    device_.journal = &journal_;
    valves_.journal = &journal_;
    spec_.journal = &journal_;
    host_.bodies["extract"] = [this](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
      failed_at_ = clock_.now();
      return fail(ErrorKind::Io, "laser link down");
    };
    alarms_sub_ = bus_.subscribe<Alarm>([this](const Alarm& a) {
      std::lock_guard lock(mutex_);
      alarms_.push_back(a);
    });
  }

  RunServices services() {
    RunServices s;
    s.clock = &clock_;
    s.bus = &bus_;
    s.scripts = &host_;
    s.resolver = &resolver_;
    s.line.device = &device_;
    s.spectrometer = &spec_;
    s.valves = &valves_;
    s.spectrometer_info = [] { return SpectrometerInfo{"hash123", "argon-2026", 1.0}; };
    s.engine.sleep = [this](pychron::Duration d) { clock_.sleep_for(d); };
    s.plans = &plans_;
    s.aliquots = &lab_.aliquots;
    s.persister = &lab_.files;
    s.save = &lab_.save;
    s.instrument.mass_spectrometer = "argus";
    return s;
  }

  std::vector<Alarm> alarms() {
    std::lock_guard lock(mutex_);
    return alarms_;
  }

  // The steps the run took after its own end of extraction.
  std::vector<std::string> safe_steps() {
    auto lines = journal_.lines();
    // extract() ends the device once itself: end_extract, disable.
    if (lines.size() >= 2) lines.erase(lines.begin(), lines.begin() + 2);
    return lines;
  }

  // The inlet B cannot be closed until `until` (Io), and for good when not given.
  void inlet_down(std::optional<TimePoint> until) {
    valves_.fault = [this, until](const std::string& v, bool open) -> Result<void> {
      if (v != "B" || open) return {};
      if (!first_close_) first_close_ = clock_.now();
      if (until && clock_.now() >= *until) {
        closed_at_ = clock_.now();
        return {};
      }
      return fail(ErrorKind::Io, "valve bus down");
    };
  }

  VirtualClock clock_{safe_clock()};
  Clock::Participant me_{clock_, "test"};
  SignalBus bus_;
  Journal journal_;
  FakeScriptHost host_;
  AnyScriptResolver resolver_;
  FakeDevice device_;
  FakeSpectrometer spec_{static_cast<const Clock&>(clock_)};
  FakeValves valves_;
  plan::PlanLibrary plans_ = test_plans();
  Lab lab_;
  QueueSpec queue_ = [] {
    QueueSpec q;
    q.name = "q1";
    q.mass_spectrometer = "argus";
    return q;
  }();
  std::mutex mutex_;
  std::vector<Alarm> alarms_;
  SignalBus::Subscription alarms_sub_;
  std::optional<TimePoint> failed_at_, first_close_, closed_at_;
};

TEST_F(SafeState, StepsRunInOrder) {
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_FALSE(r.unsafe);
  EXPECT_EQ(safe_steps(), (std::vector<std::string>{"end_extract", "disable", "close B", "unprotect CDD"}));
  EXPECT_TRUE(alarms().empty());
  EXPECT_EQ(clock_.now(), *failed_at_);  // nothing had to wait
}

TEST_F(SafeState, EachStepKeepsTryingForTheCriticalWindow) {
  inlet_down(kSafeStart + 5min);
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_FALSE(r.unsafe);
  ASSERT_TRUE(first_close_ && closed_at_);
  EXPECT_GE(*closed_at_ - kSafeStart, 5min);
  EXPECT_LE(*closed_at_ - kSafeStart, 5min + 30s);  // within one backoff of the link's return
  const auto said = alarms();
  ASSERT_EQ(said.size(), 1u);
  EXPECT_EQ(said[0].severity, AlarmSeverity::Warning);
  EXPECT_EQ(said[0].ts - *first_close_, 20s);
  EXPECT_NE(said[0].message.find("close inlet B"), std::string::npos) << said[0].message;
  EXPECT_EQ(safe_steps().back(), "unprotect CDD");
}

TEST_F(SafeState, AStepThatFailsDoesNotStopTheLaterOnes) {
  inlet_down(std::nullopt);
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  EXPECT_EQ(r.state, RunState::Failed);
  EXPECT_TRUE(r.unsafe);
  EXPECT_EQ(r.unsafe_steps, std::vector<std::string>{"close inlet B"});
  ASSERT_TRUE(first_close_);
  const auto said = alarms();
  ASSERT_EQ(said.size(), 2u);
  EXPECT_EQ(said[0].severity, AlarmSeverity::Warning);
  EXPECT_EQ(said[1].severity, AlarmSeverity::Critical);
  EXPECT_EQ(said[1].ts - *first_close_, 10min);
  EXPECT_EQ(safe_steps(), (std::vector<std::string>{"end_extract", "disable", "unprotect CDD"}));
  ASSERT_TRUE(r.record);  // saved before it was made safe
  EXPECT_EQ(r.record->provenance.status, record::RecordStatus::Failed);
}

TEST_F(SafeState, ADefiniteErrorIsNotTriedAgain) {
  valves_.fault = [](const std::string& v, bool open) -> Result<void> {
    if (v == "B" && !open) return fail(ErrorKind::Interlock, "B is interlocked");
    return {};
  };
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  EXPECT_EQ(r.unsafe_steps, std::vector<std::string>{"close inlet B"});
  EXPECT_EQ(clock_.now(), *failed_at_);
  ASSERT_EQ(alarms().size(), 1u);
  EXPECT_EQ(alarms()[0].severity, AlarmSeverity::Critical);
}

TEST_F(SafeState, ApplyingItTwiceChangesNothing) {
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  ASSERT_EQ(r.state, RunState::Failed);
  const auto once = safe_steps();
  const auto again = run.make_safe();
  EXPECT_TRUE(again.failed_steps.empty());
  auto lines = journal_.lines();
  const std::vector<std::string> second(lines.end() - 4, lines.end());
  EXPECT_EQ(second, (std::vector<std::string>{"end_extract", "disable", "close B", "unprotect CDD"}));
  EXPECT_EQ(once, second);
  EXPECT_FALSE(valves_.is_open("B"));
  EXPECT_FALSE(device_.enabled.load());
  EXPECT_TRUE(alarms().empty());
}

// ---- the lab's own safe_state script (spec 4.7) ------------------------------

TEST_F(SafeState, TheLabsScriptRunsAfterTheBuiltInSteps) {
  host_.bodies["safe_state"] = [this](const scripting::ScriptEnvironment& env,
                                      scripting::CancelToken&) -> Result<void> {
    journal_.add(env.line.device != nullptr ? "safe_state with the line" : "safe_state without the line");
    return {};
  };
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  EXPECT_FALSE(r.unsafe);
  EXPECT_EQ(safe_steps(),
            (std::vector<std::string>{"end_extract", "disable", "close B", "unprotect CDD", "safe_state with the line"}));
}

TEST_F(SafeState, ALabWithoutTheScriptRunsTheBuiltInStepsOnly) {
  resolver_.missing = {"safe_state"};
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  EXPECT_FALSE(r.unsafe);
  EXPECT_EQ(safe_steps(), (std::vector<std::string>{"end_extract", "disable", "close B", "unprotect CDD"}));
  const auto ran = host_.ran_scripts();
  EXPECT_EQ(std::count(ran.begin(), ran.end(), "safe_state"), 0);
}

TEST_F(SafeState, AScriptThatRaisesIsAFailedStep) {
  host_.bodies["safe_state"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Protocol, "NameError: name 'get_intensity' is not defined");
  };
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  EXPECT_TRUE(r.unsafe);
  EXPECT_EQ(r.unsafe_steps, std::vector<std::string>{"safe_state script"});
  const auto said = alarms();
  ASSERT_EQ(said.size(), 1u);
  EXPECT_EQ(said[0].severity, AlarmSeverity::Critical);
  EXPECT_NE(said[0].message.find("get_intensity"), std::string::npos) << said[0].message;
}

// As a build without scripting: the built-in steps still run, the script is
// not, and that is said, not counted as unsafe.
TEST_F(SafeState, WithoutScriptingTheBuiltInStepsStillRun) {
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  ASSERT_EQ(run.execute(c).state, RunState::Failed);
  host_.enabled = false;
  const auto ran_before = host_.ran_scripts().size();
  const auto again = run.make_safe();
  EXPECT_TRUE(again.failed_steps.empty());
  EXPECT_EQ(host_.ran_scripts().size(), ran_before);
  auto lines = journal_.lines();
  EXPECT_EQ(lines.back(), "unprotect CDD");
}

// There but unreadable is not "the lab has none": a failed step, said loudly.
TEST_F(SafeState, AScriptThatCannotBeReadIsAFailedStep) {
  resolver_.broken = {"safe_state"};
  AutomatedRun run(unknown_run("12345", "mcp"), queue_, services());
  RunControl c;
  const auto r = run.execute(c);
  EXPECT_TRUE(r.unsafe);
  EXPECT_EQ(r.unsafe_steps, std::vector<std::string>{"safe_state script"});
  ASSERT_EQ(alarms().size(), 1u);
  EXPECT_EQ(alarms()[0].severity, AlarmSeverity::Critical);
}

// ---- a silence in the readings, as the run keeps it (spec 4.4) ---------------

TEST_F(RunTest, AGapIsInTheRecordAndSaid) {
  std::vector<RunNote> said;
  auto sub = bus_.subscribe<RunNote>([&](const RunNote& n) { said.push_back(n); });
  // Link down from the 6th reading, for good: waited out, the acquisition
  // started again once, then main ends with what it has.
  spec_.fail_kind = ErrorKind::Io;
  spec_.fail_from = 6;
  auto r = go(unknown_run("12345"));
  ASSERT_TRUE(r.record);
  ASSERT_FALSE(r.measurement.gaps.empty());
  const auto gap = std::find_if(r.record->events.begin(), r.record->events.end(),
                                [](const record::Event& e) { return e.kind == "gap"; });
  ASSERT_NE(gap, r.record->events.end());
  EXPECT_EQ(gap->detail, measurement::describe(r.measurement.gaps.front()));
  EXPECT_TRUE(std::any_of(said.begin(), said.end(), [&](const RunNote& n) { return n.text == gap->detail; }));
  // Said once as a note, kept once as a gap: not again as a note event.
  EXPECT_EQ(std::count_if(r.record->events.begin(), r.record->events.end(),
                          [&](const record::Event& e) { return e.detail == gap->detail; }),
            1);
}
