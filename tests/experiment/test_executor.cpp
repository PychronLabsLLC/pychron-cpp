// Executor (experiment spec 3.2, 3.3) over fakes: queue loop, delay policy,
// stop/cancel/abort/truncate, end_after, conditionals acting on the queue,
// pre-run checks, failures, resume, pause, scheduled start, overlap and
// editing the queue while it runs.

#include <gtest/gtest.h>

#include <filesystem>

#include "pychron/experiment/executor/executor.hpp"
#include "run_fakes.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::executor;
using namespace pychron::experiment::fakes;
using Seconds = pychron::experiment::Duration;

namespace {

class FailingCheck final : public IPreRunCheck {
 public:
  std::string name() const override { return "disk"; }
  Result<void> check(const RunSpec& run) override {
    ++calls;
    if (run.id.identifier == fail_on) return fail(ErrorKind::Io, "disk full");
    return {};
  }
  std::string fail_on;
  int calls = 0;
};

class ExecutorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    subs_.push_back(bus_.subscribe<ExecutorWaiting>([this](const ExecutorWaiting& e) {
      std::lock_guard lock(mutex_);
      waits_.emplace_back(e.reason, e.duration.count());
    }));
    subs_.push_back(bus_.subscribe<RunStarted>([this](const RunStarted& e) {
      std::lock_guard lock(mutex_);
      order_.push_back("start " + e.identifier);
    }));
    subs_.push_back(bus_.subscribe<RunFinished>([this](const RunFinished& e) {
      std::lock_guard lock(mutex_);
      order_.push_back("finish " + e.summary.identifier);
    }));
  }

  ExecutorContext context() {
    ExecutorContext c;
    auto& s = c.services;
    s.clock = &clock_;
    s.bus = &bus_;
    s.scripts = &host_;
    s.resolver = &resolver_;
    s.line.device = &device_;
    s.spectrometer = &spec_;
    s.valves = &valves_;
    s.spectrometer_info = [] { return run::SpectrometerInfo{"hash", "argon", 1.0}; };
    s.engine.sleep = [this](pychron::Duration d) { clock_.advance(d); };
    s.plans = &plans_;
    s.conditionals = &library_;
    s.aliquots = &lab_.aliquots;
    s.persister = &lab_.files;
    s.save = &lab_.save;
    s.instrument.mass_spectrometer = "argus";
    c.checks = {&check_};
    return c;
  }

  ExecutorOptions options() {
    ExecutorOptions o;
    o.state_file = lab_.dir() / "executor_state.json";
    o.sleep = [this](Seconds d) { clock_.advance(std::chrono::duration_cast<pychron::Duration>(d)); };
    return o;
  }

  ExperimentQueue queue(std::vector<RunSpec> runs, std::string conditionals = "") {
    QueueSpec q;
    q.name = "q1";
    q.mass_spectrometer = "argus";
    q.queue_conditionals = std::move(conditionals);
    q.delays.before_analyses = Seconds{10};
    q.delays.between_analyses = Seconds{20};
    q.delays.after_blank = Seconds{30};
    q.runs = std::move(runs);
    return ExperimentQueue(std::move(q));
  }

  std::vector<std::string> states(const QueueResult& r) {
    std::vector<std::string> out;
    for (const auto& s : r.runs) out.push_back(s.identifier + ":" + std::string(run::to_string(s.state)));
    return out;
  }

  std::vector<std::pair<std::string, double>> waits() {
    std::lock_guard lock(mutex_);
    return waits_;
  }
  std::vector<std::string> order() {
    std::lock_guard lock(mutex_);
    return order_;
  }

  ManualClock clock_{TimePoint{} + 1000s};
  SignalBus bus_;
  FakeScriptHost host_;
  AnyScriptResolver resolver_;
  FakeDevice device_;
  FakeSpectrometer spec_{clock_};
  FakeValves valves_;
  plan::PlanLibrary plans_ = test_plans();
  MapConditionalSource source_;
  ConditionalLibrary library_{source_};
  FailingCheck check_;
  Lab lab_;
  std::mutex mutex_;
  std::vector<std::pair<std::string, double>> waits_;
  std::vector<std::string> order_;
  std::vector<SignalBus::Subscription> subs_;
};

TEST_F(ExecutorTest, RunsTheQueueSeriallyWithTheDelayPolicy) {
  RunSpec blank = unknown_run("bu");
  blank.id.type = AnalysisType::BlankUnknown;
  RunSpec skipped = unknown_run("skipped");
  skipped.skip = true;
  RunSpec custom = unknown_run("12346");
  custom.delay_after = Seconds{5};
  auto q = queue({unknown_run("12345"), skipped, blank, unknown_run("12345"), custom, unknown_run("12347")});
  Executor ex(context(), options());
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states(r), (std::vector<std::string>{"12345:success", "bu:success", "12345:success", "12346:success",
                                                 "12347:success"}));
  EXPECT_EQ(r.runs[0].aliquot, 1);
  EXPECT_EQ(r.runs[2].aliquot, 2);
  std::vector<double> delays;
  for (const auto& [reason, d] : waits())
    if (reason.starts_with("delay")) delays.push_back(d);
  // before_analyses; after 12345 between; after the blank after_blank; between; 12346's own delay_after.
  EXPECT_EQ(delays, (std::vector<double>{10, 20, 30, 20, 5}));
  EXPECT_EQ(ex.state(), ExecutorState::Idle);
  EXPECT_EQ(*Executor::resume_row(lab_.dir() / "executor_state.json"), 6u);
  EXPECT_EQ(check_.calls, 5);
  EXPECT_EQ(spec_.overlapping.load(), 0);
}

// A run's log reaches whoever follows the queue: in its summary, and so in
// RunFinished and the queue's result.
TEST_F(ExecutorTest, ARunsLogIsInItsSummary) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    env.log("hole 3: not centred (no_target); at its calibrated position");
    return {};
  };
  std::vector<RunSummary> finished;
  subs_.push_back(bus_.subscribe<RunFinished>([&](const RunFinished& e) { finished.push_back(e.summary); }));
  auto q = queue({unknown_run("12345")});
  Executor ex(context(), options());
  const auto result = ex.execute(q);
  ASSERT_EQ(result.runs.size(), 1u);
  const std::vector<std::string> said{"hole 3: not centred (no_target); at its calibrated position"};
  EXPECT_EQ(result.runs[0].messages, said);
  ASSERT_EQ(finished.size(), 1u);
  EXPECT_EQ(finished[0].messages, said);
}

TEST_F(ExecutorTest, EndAfterStopsAtTheBoundary) {
  auto first = unknown_run("12345");
  first.end_after = true;
  auto q = queue({first, unknown_run("12346")});
  Executor ex(context(), options());
  auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Stopped);
  EXPECT_EQ(r.runs.size(), 1u);
}

TEST_F(ExecutorTest, StopFinishesTheCurrentRun) {
  Executor ex(context(), options());
  spec_.on_reading = [&](int n) {
    if (n == 3) ex.stop();
  };
  auto q = queue({unknown_run("12345"), unknown_run("12346")});
  auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Stopped);
  EXPECT_EQ(states(r), std::vector<std::string>{"12345:success"});
}

TEST_F(ExecutorTest, CancelAndAbortEndTheQueue) {
  {
    Executor ex(context(), options());
    spec_.on_reading = [&](int n) {
      if (n == 3) ex.cancel();
    };
    auto q = queue({unknown_run("12345"), unknown_run("12346")});
    auto r = ex.execute(q);
    EXPECT_EQ(r.end, QueueEnd::Cancelled);
    EXPECT_EQ(states(r), std::vector<std::string>{"12345:cancelled"});
  }
  {
    Executor ex(context(), options());
    spec_.readings = 0;
    spec_.on_reading = [&](int n) {
      if (n == 3) ex.abort();
    };
    auto q = queue({unknown_run("22345"), unknown_run("22346")});
    auto r = ex.execute(q);
    EXPECT_EQ(r.end, QueueEnd::Aborted);
    EXPECT_EQ(states(r), std::vector<std::string>{"22345:aborted"});
    EXPECT_FALSE(valves_.is_open("B"));
  }
}

TEST_F(ExecutorTest, TruncateReachesTheMeasuringRun) {
  Executor ex(context(), options());
  spec_.on_reading = [&](int n) {
    if (n == 7) ex.truncate();
  };
  auto q = queue({unknown_run("12345")});
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_TRUE(r.runs[0].truncated);
}

TEST_F(ExecutorTest, CancelationConditionalCancelsTheQueue) {
  source_.add("q-cond", R"(
[[cancelations]]
name = "too_big"
check = "Ar40.cur > 0"
start = 2
)");
  auto q = queue({unknown_run("12345"), unknown_run("12346")}, "q-cond");
  Executor ex(context(), options());
  auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Cancelled);
  EXPECT_NE(r.reason.find("too_big"), std::string::npos) << r.reason;
  EXPECT_EQ(r.runs.size(), 1u);
}

TEST_F(ExecutorTest, ModificationAndPostRunConditionalsChangeTheQueue) {
  source_.add("q-mod", R"(
[[modifications]]
name = "blank_next"
check = "Ar40.cur > 0"
start = 2
action = "run_blank"
analysis_types = ["unknown"]

[[post_run]]
name = "skip_after_blank"
check = "Ar40 < 1e9"
action = "skip_next"
analysis_types = ["blank_unknown"]
)");
  auto q = queue({unknown_run("12345"), unknown_run("12346"), unknown_run("12347")}, "q-mod");
  std::vector<QueueEdited> edits;
  auto sub = bus_.subscribe<QueueEdited>([&](const QueueEdited& e) { edits.push_back(e); });
  Executor ex(context(), options());
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  // One snapshot per run that changed the queue (the last blank changed nothing).
  ASSERT_EQ(edits.size(), 3u);
  EXPECT_EQ(edits[0].changes, std::vector<std::string>{"blank_next: run_blank"});
  EXPECT_EQ(edits[0].queue.runs.size(), 4u);
  EXPECT_EQ(edits[0].queue.runs[1].id.identifier, "bu");
  EXPECT_TRUE(edits[1].queue.runs[2].skip);
  EXPECT_EQ(edits.back().queue, q.spec());
  // Every change bumps the version an operator edit is checked against.
  EXPECT_EQ(edits[0].version, 1u);
  EXPECT_EQ(edits[2].version, 3u);
  EXPECT_EQ(ex.queue_version(), 3u);
  // 12345 inserts a blank; the blank's post-run check skips 12346; 12347 inserts
  // another blank, whose post-run check has nothing left to skip.
  EXPECT_EQ(states(r),
            (std::vector<std::string>{"12345:success", "bu:success", "12347:success", "bu:success"}));
  ASSERT_EQ(r.runs[0].queue_changes.size(), 1u);
  EXPECT_EQ(r.runs[0].queue_changes[0], "blank_next: run_blank");
  EXPECT_EQ(r.runs[1].queue_changes, std::vector<std::string>{"skip_after_blank: skip_next"});
  EXPECT_TRUE(q.runs()[2].skip);
  EXPECT_EQ(q.runs()[2].id.identifier, "12346");
}

TEST_F(ExecutorTest, AnEditChangesTheRowsAfterTheRunningOne) {
  auto q = queue({unknown_run("12345"), unknown_run("12346"), unknown_run("12347")});
  std::vector<QueueEdited> edits;
  std::vector<std::size_t> frontier;
  auto sub = bus_.subscribe<QueueEdited>([&](const QueueEdited& e) { edits.push_back(e); });
  auto sub2 = bus_.subscribe<QueueFrontier>([&](const QueueFrontier& e) { frontier.push_back(e.frozen); });
  Executor ex(context(), options());
  EXPECT_FALSE(ex.edit(0, {}));  // nothing running
  std::optional<Result<std::uint64_t>> result;
  std::size_t frozen = 0;
  spec_.on_reading = [&](int n) {
    if (n != 3) return;
    frozen = ex.frozen_rows();
    std::vector<RunSpec> runs = q.spec().runs;  // the measuring thread may read it: the executor is waiting
    runs[1].id.identifier = "22346";
    runs[2] = unknown_run("22347");
    runs.push_back(unknown_run("22348"));
    result = ex.edit(ex.queue_version(), runs, "operator");
  };
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(frozen, 1u);
  ASSERT_TRUE(result && *result) << (result && !*result ? result->error().what : "");
  EXPECT_EQ(**result, 1u);
  EXPECT_EQ(states(r), (std::vector<std::string>{"12345:success", "22346:success", "22347:success",
                                                 "22348:success"}));
  ASSERT_EQ(edits.size(), 1u);
  EXPECT_EQ(edits[0].changes, std::vector<std::string>{"operator"});
  EXPECT_EQ(edits[0].version, 1u);
  EXPECT_EQ(edits[0].frozen, 1u);
  EXPECT_EQ(edits[0].queue.runs.size(), 4u);
  EXPECT_EQ(frontier, (std::vector<std::size_t>{1, 2, 3, 4}));
  EXPECT_FALSE(ex.edit(1, q.spec().runs));  // finished
}

TEST_F(ExecutorTest, AnEditMayNotChangeReachedRowsOrUseAStaleVersion) {
  auto q = queue({unknown_run("12345"), unknown_run("12346")});
  Executor ex(context(), options());
  std::vector<std::string> errors;
  spec_.on_reading = [&](int n) {
    if (n != 3) return;
    auto runs = q.spec().runs;
    runs[0].comment = "changed";
    if (auto e = ex.edit(0, runs); !e) errors.push_back(e.error().what);
    if (auto e = ex.edit(0, {}); !e) errors.push_back(e.error().what);
    if (auto e = ex.edit(7, q.spec().runs); !e) errors.push_back(e.error().what);
  };
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  ASSERT_EQ(errors.size(), 3u);
  EXPECT_NE(errors[0].find("row 0 has already been reached"), std::string::npos) << errors[0];
  EXPECT_NE(errors[1].find("removes rows"), std::string::npos) << errors[1];
  EXPECT_NE(errors[2].find("changed while it was being edited"), std::string::npos) << errors[2];
  EXPECT_EQ(states(r), (std::vector<std::string>{"12345:success", "12346:success"}));
  EXPECT_EQ(ex.queue_version(), 0u);
}

TEST_F(ExecutorTest, ARowEditedDuringItsDelayIsReadAgain) {
  auto q = queue({unknown_run("12345"), unknown_run("12346"), unknown_run("12347")});
  auto o = options();
  Executor* ex = nullptr;
  std::optional<Result<std::uint64_t>> result;
  std::size_t frozen = 0;
  o.sleep = [&](Seconds d) {
    // The delay before 12346 (between_analyses): that row is not reached yet.
    if (d == Seconds{20} && !result) {
      frozen = ex->frozen_rows();
      auto runs = q.spec().runs;
      runs[1].skip = true;
      result = ex->edit(ex->queue_version(), runs);
    }
    clock_.advance(std::chrono::duration_cast<pychron::Duration>(d));
  };
  Executor executor(context(), o);
  ex = &executor;
  auto r = executor.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(frozen, 1u);
  ASSERT_TRUE(result && *result);
  EXPECT_EQ(states(r), (std::vector<std::string>{"12345:success", "12347:success"}));
  // 12347 starts after the delay already waited, not after a second one.
  std::vector<double> delays;
  for (const auto& [reason, d] : waits())
    if (reason.starts_with("delay")) delays.push_back(d);
  EXPECT_EQ(delays, (std::vector<double>{10, 20}));
}

TEST_F(ExecutorTest, PreRunChecksAndConditionalsBlockTheQueue) {
  check_.fail_on = "12346";
  auto q = queue({unknown_run("12345"), unknown_run("12346"), unknown_run("12347")});
  Executor ex(context(), options());
  auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Cancelled);
  EXPECT_NE(r.reason.find("pre-run check 'disk': disk full"), std::string::npos) << r.reason;
  EXPECT_EQ(r.runs.size(), 1u);

  check_.fail_on.clear();
  source_.add("system", "[[pre_run]]\nname = \"cdd\"\ncheck = \"CDD.inactive\"\n");
  MapContext metrics;
  metrics.series_data["CDD.inactive"] = {1};
  auto ctx = context();
  ctx.pre_run_metrics = &metrics;
  Executor ex2(ctx, options());
  auto q2 = queue({unknown_run("22345")});
  auto r2 = ex2.execute(q2);
  EXPECT_EQ(r2.end, QueueEnd::Cancelled);
  EXPECT_NE(r2.reason.find("pre_run conditional 'cdd'"), std::string::npos) << r2.reason;
  EXPECT_TRUE(r2.runs.empty());
}

TEST_F(ExecutorTest, FailedRunEndsTheQueueUnlessToldToContinue) {
  auto q = queue({unknown_run("12345", "missing_plan"), unknown_run("12346")});
  Executor ex(context(), options());
  auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Failed);
  EXPECT_NE(r.reason.find("missing_plan"), std::string::npos) << r.reason;
  EXPECT_EQ(r.runs.size(), 1u);

  auto opts = options();
  opts.continue_on_failure = true;
  Executor ex2(context(), opts);
  auto q2 = queue({unknown_run("22345", "missing_plan"), unknown_run("22346")});
  auto r2 = ex2.execute(q2);
  EXPECT_EQ(r2.end, QueueEnd::Completed);
  EXPECT_EQ(states(r2), (std::vector<std::string>{"22345:failed", "22346:success"}));
}

TEST_F(ExecutorTest, ResumeStartsAfterTheLastStartedRun) {
  Executor ex(context(), options());
  spec_.on_reading = [&](int n) {
    if (n == 3) ex.abort();  // e.g. a crash mid-measurement
  };
  auto q = queue({unknown_run("12345"), unknown_run("12346"), unknown_run("12347")});
  auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Aborted);
  auto from = Executor::resume_row(lab_.dir() / "executor_state.json");
  ASSERT_TRUE(from) << from.error().what;
  EXPECT_EQ(*from, 1u);  // the partially measured 12345 is not re-run

  spec_.on_reading = nullptr;
  Executor again(context(), options());
  auto r2 = again.execute(q, *from);
  EXPECT_EQ(r2.end, QueueEnd::Completed) << r2.reason;
  EXPECT_EQ(states(r2), (std::vector<std::string>{"12346:success", "12347:success"}));
  EXPECT_FALSE(Executor::resume_row(lab_.dir() / "nope.json"));
}

TEST_F(ExecutorTest, PauseAndScheduledStart) {
  RunSpec pause;
  pause.id.identifier = "pa";
  pause.id.type = AnalysisType::Pause;
  pause.extraction.duration = Seconds{120};
  auto q = queue({unknown_run("12345"), pause, unknown_run("12346")});
  auto opts = options();
  opts.start_at = clock_.now() + 600s;
  Executor ex(context(), opts);
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(r.runs.size(), 2u);
  const auto w = waits();
  ASSERT_FALSE(w.empty());
  EXPECT_EQ(w.front(), (std::pair<std::string, double>{"scheduled start", 600}));
  bool paused = false;
  for (const auto& [reason, d] : w) paused |= reason == "pause" && d == 120;
  EXPECT_TRUE(paused);
}

TEST_F(ExecutorTest, StopAtEndsAtABoundary) {
  auto opts = options();
  opts.stop_at = clock_.now() + 30s;  // passes during the first run
  Executor ex(context(), opts);
  auto q = queue({unknown_run("12345"), unknown_run("12346")});
  auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Stopped);
  EXPECT_EQ(r.runs.size(), 1u);
}

TEST_F(ExecutorTest, OverlapStartsTheNextRunBeforeThisOneFinishes) {
  RunSpec first = unknown_run("12345");
  first.overlap.duration = Seconds{2};
  first.overlap.min_delay = Seconds{1};
  RunSpec second = unknown_run("12346");
  second.overlap.min_delay = Seconds{3};
  // Fake hardware finishes a run in milliseconds; a real one keeps measuring
  // baselines for minutes after its inlet closes. Hold the first run in
  // post-measurement until the second has started so the overlap is
  // deterministic under load.
  host_.bodies["post_meas"] = [this](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    const auto id = std::get<std::string>(env.context.globals.at("run_identifier"));
    if (!id.starts_with("12345")) return {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto o = order();
      if (std::find(o.begin(), o.end(), "start 12346") != o.end()) return {};
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return fail(ErrorKind::Timeout, "the second run never started");
  };
  auto q = queue({first, second});
  Executor ex(context(), options());
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states(r), (std::vector<std::string>{"12345:success", "12346:success"}));
  const auto o = order();
  ASSERT_EQ(o.size(), 4u);
  EXPECT_EQ(o, (std::vector<std::string>{"start 12345", "start 12346", "finish 12345", "finish 12346"}));
  EXPECT_EQ(spec_.overlapping.load(), 0);  // never two measurements at once
  bool overlap_wait = false;
  for (const auto& [reason, d] : waits()) overlap_wait |= reason == "overlap" && d == 2;
  EXPECT_TRUE(overlap_wait);
  // Only one delay: the next run started inside the previous one.
  int delays = 0;
  for (const auto& [reason, d] : waits()) delays += reason.starts_with("delay") ? 1 : 0;
  EXPECT_EQ(delays, 1);
}

TEST_F(ExecutorTest, LastRunAndNonUnknownsNeverOverlap) {
  RunSpec only = unknown_run("12345");
  only.overlap.duration = Seconds{2};
  RunSpec air = unknown_run("a");
  air.id.type = AnalysisType::Air;
  air.overlap.duration = Seconds{2};
  auto q = queue({air, only});
  Executor ex(context(), options());
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(order(), (std::vector<std::string>{"start a", "finish a", "start 12345", "finish 12345"}));
}

}  // namespace
