// Executor (experiment spec 3.2, 3.3) over fakes: queue loop, delay policy,
// stop/cancel/abort/truncate, end_after, conditionals acting on the queue,
// pre-run checks, failures, resume, pause, scheduled start, overlap and
// editing the queue while it runs.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/executor/links.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "run_fakes.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::executor;
using namespace pychron::experiment::fakes;
using Seconds = pychron::experiment::Duration;

namespace {

class FailingCheck final : public IPreRunCheck {
 public:
  std::string name() const override { return "disk"; }
  std::optional<CheckFailure> check(const RunSpec& run) override {
    ++calls;
    if (run.id.identifier == fail_on) return CheckFailure{CheckKind::Refuse, Error{ErrorKind::Io, "disk full", {}}, {}};
    return std::nullopt;
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

// The steps of one heating are one aliquot: a step heat made by the run
// factory leaves the aliquot to run start, and steps B and C must not each
// take the next free one.
TEST_F(ExecutorTest, TheStepsOfAStepHeatShareOneAliquot) {
  std::vector<RunSpec> runs;
  for (const char* step : {"A", "B", "C"}) {
    runs.push_back(unknown_run("12345"));
    runs.back().id.step = step;
  }
  auto q = queue(std::move(runs));
  Executor ex(context(), options());
  auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  ASSERT_EQ(r.runs.size(), 3u);
  EXPECT_EQ(r.runs[0].aliquot, 1);
  EXPECT_EQ(r.runs[1].aliquot, 1);
  EXPECT_EQ(r.runs[2].aliquot, 1);
}

// A run's log reaches whoever follows the queue: in its summary, and so in
// RunFinished and the queue's result.
TEST_F(ExecutorTest, ARunsLogIsInItsSummary) {
  host_.bodies["extract"] = [](const scripting::ScriptEnvironment& env, scripting::CancelToken&) -> Result<void> {
    env.log("hole 3: not centered (no_target); at its calibrated position");
    return {};
  };
  std::vector<RunSummary> finished;
  subs_.push_back(bus_.subscribe<RunFinished>([&](const RunFinished& e) { finished.push_back(e.summary); }));
  auto q = queue({unknown_run("12345")});
  Executor ex(context(), options());
  const auto result = ex.execute(q);
  ASSERT_EQ(result.runs.size(), 1u);
  const std::vector<std::string> said{"hole 3: not centered (no_target); at its calibrated position"};
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

// A request may come before the queue's thread has got as far as execute()
// (LabSession starts that thread and returns): it is not lost.
TEST_F(ExecutorTest, ARequestMadeBeforeExecuteIsHonoured) {
  {
    Executor ex(context(), options());
    ex.abort();
    auto q = queue({unknown_run("12345"), unknown_run("12346")});
    auto r = ex.execute(q);
    EXPECT_EQ(r.end, QueueEnd::Aborted) << r.reason;
    EXPECT_TRUE(r.runs.empty());
  }
  {
    Executor ex(context(), options());
    ex.cancel();
    auto q = queue({unknown_run("22345"), unknown_run("22346")});
    auto r = ex.execute(q);
    EXPECT_EQ(r.end, QueueEnd::Cancelled) << r.reason;
    EXPECT_TRUE(r.runs.empty());
  }
  {
    Executor ex(context(), options());
    ex.stop();
    auto q = queue({unknown_run("32345"), unknown_run("32346")});
    auto r = ex.execute(q);
    EXPECT_EQ(r.end, QueueEnd::Stopped) << r.reason;
    EXPECT_TRUE(r.runs.empty());
  }
  EXPECT_EQ(spec_.readings.load(), 0) << "nothing was measured";
  EXPECT_TRUE(order().empty());
}

// The request belongs to the queue it ended: the next one runs.
TEST_F(ExecutorTest, ARequestEndsOneQueueOnly) {
  Executor ex(context(), options());
  ex.abort();
  auto q = queue({unknown_run("12345")});
  EXPECT_EQ(ex.execute(q).end, QueueEnd::Aborted);
  EXPECT_EQ(ex.state(), ExecutorState::Idle);
  auto q2 = queue({unknown_run("12346")});
  auto r = ex.execute(q2);
  EXPECT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states(r), std::vector<std::string>{"12346:success"});
}

TEST(ExecutorStates, OnlyTheTransitionsOfAQueueAreLegal) {
  using S = ExecutorState;
  const std::vector<std::pair<S, S>> legal = {
      {S::Idle, S::Preparing},
      {S::Preparing, S::Running},
      {S::Preparing, S::StoppingAtBoundary},
      {S::Preparing, S::Cancelling},
      {S::Preparing, S::Aborting},
      {S::Preparing, S::Finalizing},
      {S::Running, S::StoppingAtBoundary},
      {S::Running, S::Cancelling},
      {S::Running, S::Aborting},
      {S::Running, S::Finalizing},
      {S::StoppingAtBoundary, S::Cancelling},
      {S::StoppingAtBoundary, S::Aborting},
      {S::StoppingAtBoundary, S::Finalizing},
      {S::Cancelling, S::Aborting},
      {S::Cancelling, S::Finalizing},
      {S::Aborting, S::Finalizing},
      {S::Finalizing, S::Idle},
  };
  const std::array all = {S::Idle,       S::Preparing, S::Running,   S::StoppingAtBoundary,
                          S::Cancelling, S::Aborting,  S::Finalizing};
  for (auto from : all) {
    for (auto to : all) {
      const bool expected = std::find(legal.begin(), legal.end(), std::pair(from, to)) != legal.end();
      EXPECT_EQ(can_transition(from, to), expected) << to_string(from) << " -> " << to_string(to);
    }
  }
}

TEST_F(ExecutorTest, TheStatesOfAQueueArePublishedInOrder) {
  std::vector<std::string> seen;
  auto sub = bus_.subscribe<ExecutorStateChanged>(
      [&](const ExecutorStateChanged& e) { seen.emplace_back(to_string(e.to)); });
  auto q = queue({unknown_run("12345")});
  Executor ex(context(), options());
  ASSERT_EQ(ex.execute(q).end, QueueEnd::Completed);
  EXPECT_EQ(seen, (std::vector<std::string>{"preparing", "running", "finalizing", "idle"}));
}

// A weaker request never replaces a stronger one: the queue is still being
// cancelled, whatever is asked for afterwards.
TEST_F(ExecutorTest, ALaterStopDoesNotHideACancel) {
  std::vector<std::string> seen;
  auto sub = bus_.subscribe<ExecutorStateChanged>(
      [&](const ExecutorStateChanged& e) { seen.emplace_back(to_string(e.to)); });
  Executor ex(context(), options());
  spec_.on_reading = [&](int n) {
    if (n != 3) return;
    ex.cancel();
    ex.stop();
    EXPECT_EQ(ex.state(), ExecutorState::Cancelling);
  };
  auto q = queue({unknown_run("12345"), unknown_run("12346")});
  EXPECT_EQ(ex.execute(q).end, QueueEnd::Cancelled);
  EXPECT_EQ(seen, (std::vector<std::string>{"preparing", "running", "cancelling", "finalizing", "idle"}));
}

// A request made with no queue running is kept for the next one, and until
// then the executor is what it is: idle.
TEST_F(ExecutorTest, ARequestWithNoQueueRunningLeavesTheExecutorIdle) {
  std::vector<std::string> seen;
  auto sub = bus_.subscribe<ExecutorStateChanged>(
      [&](const ExecutorStateChanged& e) { seen.emplace_back(to_string(e.to)); });
  Executor ex(context(), options());
  ex.stop();
  EXPECT_EQ(ex.state(), ExecutorState::Idle);
  EXPECT_TRUE(seen.empty());
  auto q = queue({unknown_run("12345")});
  auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Stopped);
  EXPECT_TRUE(r.runs.empty());
  EXPECT_EQ(seen, (std::vector<std::string>{"preparing", "stopping", "finalizing", "idle"}));
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

// ---- On a VirtualClock -----------------------------------------------------------
//
// The same fakes, with nothing advancing time by hand: scripts and readings
// wait on the clock, and the clock jumps when every thread of the queue is
// asleep in it.

const TimePoint kStart = TimePoint{} + std::chrono::hours(1);
const WallTime kEpoch = std::chrono::sys_days{std::chrono::year{2026} / 10 / 6} + std::chrono::hours(12);

// "2026-10-06T12:00:10Z", as a run stamps itself.
std::chrono::sys_seconds parse_utc(const std::string& s) {
  if (s.size() != 20) return {};
  const auto n = [&](std::size_t at, std::size_t len) { return std::stoi(s.substr(at, len)); };
  const auto day = std::chrono::year{n(0, 4)} / static_cast<unsigned>(n(5, 2)) / static_cast<unsigned>(n(8, 2));
  return std::chrono::sys_days{day} + std::chrono::hours(n(11, 2)) + std::chrono::minutes(n(14, 2)) +
         std::chrono::seconds(n(17, 2));
}

// The file persister, remembering each run's timestamp as its extraction is saved.
class StampingPersister final : public persist::IAnalysisPersister {
 public:
  explicit StampingPersister(persist::IAnalysisPersister& to) : to_(to) {}
  Result<int> next_aliquot(const std::string& identifier) override { return to_.next_aliquot(identifier); }
  Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override {
    if (fail_begins > 0) {  // a store whose link is down, before anything of the sample is spent
      --fail_begins;
      return fail(ErrorKind::Io, "store link down");
    }
    return to_.begin_run(id, queue);
  }
  std::atomic<int> fail_begins{0};
  Result<void> save_extraction(const record::AnalysisRecord& record) override {
    {
      std::lock_guard lock(mutex_);
      stamps_.push_back(record.identity.timestamp);
    }
    return to_.save_extraction(record);
  }
  Result<void> save_analysis(const record::AnalysisRecord& record) override { return to_.save_analysis(record); }
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override {
    return to_.save_artifact(uuid, name, bytes);
  }
  Result<void> flush() override { return to_.flush(); }
  Result<std::vector<std::string>> check_queue(const QueueSpec& queue) override { return to_.check_queue(queue); }
  std::vector<std::string> stamps() {
    std::lock_guard lock(mutex_);
    return stamps_;
  }

 private:
  persist::IAnalysisPersister& to_;
  std::mutex mutex_;
  std::vector<std::string> stamps_;
};

// A script that takes `d` on the run's clock, as a script's sleep() does.
FakeScriptHost::Body takes(pychron::Duration d) {
  return [d](const scripting::ScriptEnvironment& env, scripting::CancelToken& token) -> Result<void> {
    token.wait_until(*env.clock, env.clock->now() + d);
    return {};
  };
}

// A run whose main block takes `counts` seconds of readings.
RunSpec counted_run(const std::string& identifier, std::int64_t counts) {
  RunSpec r = unknown_run(identifier);
  r.measurement.overrides["main.hops[0].counts"] = counts;
  return r;
}

// What a queue runs on, on a clock of the test's making.
struct World {
  explicit World(VirtualClock::Options o) : clock(reporting(std::move(o))) {
    subs.push_back(bus.subscribe<RunStarted>([this](const RunStarted& e) {
      std::lock_guard lock(mutex);
      started.push_back(e);
    }));
    subs.push_back(bus.subscribe<run::RunStateChanged>([this](const run::RunStateChanged& e) {
      std::lock_guard lock(mutex);
      changes.push_back(e);
    }));
    subs.push_back(bus.subscribe<measurement::BlockStarted>([this](const measurement::BlockStarted& e) {
      std::lock_guard lock(mutex);
      blocks.emplace_back(e.run_id, clock.now());  // published by the run's thread: time stands
    }));
  }

  VirtualClock::Options reporting(VirtualClock::Options o) {
    o.start = kStart;
    o.epoch = kEpoch;
    o.on_stall = [this](std::string what) {
      std::lock_guard lock(mutex);
      stalls.push_back(std::move(what));
    };
    return o;
  }

  ExecutorContext context() {
    ExecutorContext c;
    auto& s = c.services;
    s.clock = &clock;
    s.bus = &bus;
    s.scripts = &host;
    s.resolver = &resolver;
    s.line.device = &device;
    s.spectrometer = &spec;
    s.valves = &valves;
    s.spectrometer_info = [] { return run::SpectrometerInfo{"hash", "argon", 1.0}; };
    s.plans = &plans;
    s.conditionals = &library;
    s.aliquots = &lab.aliquots;
    s.persister = &persister;
    s.save = &lab.save;
    s.instrument.mass_spectrometer = "argus";
    return c;
  }

  ExecutorOptions options() {
    ExecutorOptions o;
    o.state_file = lab.dir() / "executor_state.json";
    return o;
  }

  ExperimentQueue queue(std::vector<RunSpec> runs, Seconds before, Seconds between) {
    QueueSpec q;
    q.name = "q1";
    q.mass_spectrometer = "argus";
    q.delays.before_analyses = before;
    q.delays.between_analyses = between;
    q.runs = std::move(runs);
    return ExperimentQueue(std::move(q));
  }

  // Three runs of 300 s of extraction and 300 s of main block (and a few
  // seconds of settling, equilibration and baseline) each.
  ExperimentQueue three_runs(Seconds before = Seconds{10}) {
    host.bodies["extract"] = takes(300s);
    return queue({counted_run("12345", 300), counted_run("12346", 300), counted_run("12347", 300)}, before,
                 Seconds{60});
  }

  // Two runs, the first letting the second start 2 s after its inlet closes.
  // Each extracts for 300 s; the first then measures for some 600 s, so the
  // second is ready for the spectrometer about 300 s before it is free.
  // No minimum pump time: the wait is for the spectrometer alone.
  ExperimentQueue overlapped_runs() {
    host.bodies["extract"] = takes(300s);
    RunSpec first = counted_run("12345", 600);
    first.overlap.duration = Seconds{2};
    return queue({first, unknown_run("12346")}, Seconds{10}, Seconds{60});
  }

  std::string run_id(const std::string& identifier) {
    std::lock_guard lock(mutex);
    for (const auto& e : started)
      if (e.identifier == identifier) return e.run_id;
    return {};
  }
  // When run `id` entered `state`; nullopt if it has not.
  std::optional<TimePoint> entered(const std::string& id, run::RunState state) {
    std::lock_guard lock(mutex);
    for (const auto& e : changes)
      if (e.run_id == id && e.to == state) return e.ts;
    return std::nullopt;
  }
  // When run `id`'s measurement began its first block; nullopt if it has not.
  std::optional<TimePoint> first_block(const std::string& id) {
    std::lock_guard lock(mutex);
    for (const auto& [run, ts] : blocks)
      if (run == id) return ts;
    return std::nullopt;
  }
  std::vector<RunStarted> starts() {
    std::lock_guard lock(mutex);
    return started;
  }
  std::vector<std::string> stall_reports() {
    std::lock_guard lock(mutex);
    return stalls;
  }

  // Before the clock: its watchdog reports into these.
  std::mutex mutex;
  std::vector<std::string> stalls;
  VirtualClock clock;
  SignalBus bus;
  FakeScriptHost host;
  AnyScriptResolver resolver;
  FakeDevice device;
  FakeSpectrometer spec{static_cast<const Clock&>(clock)};
  FakeValves valves;
  plan::PlanLibrary plans = test_plans();
  MapConditionalSource source;
  ConditionalLibrary library{source};
  Lab lab;
  StampingPersister persister{lab.files};
  std::vector<RunStarted> started;
  std::vector<run::RunStateChanged> changes;
  std::vector<std::pair<std::string, TimePoint>> blocks;
  std::vector<SignalBus::Subscription> subs;
};

std::vector<std::string> states_of(const QueueResult& r) {
  std::vector<std::string> out;
  for (const auto& s : r.runs) out.push_back(s.identifier + ":" + std::string(run::to_string(s.state)));
  return out;
}

double real_seconds_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

using ExecutorVirtual = pychron::testing::VirtualTimeTest;

TEST_F(ExecutorVirtual, QueueOfThreeRunsTakesNoRealTime) {
  World w({});
  Clock::Participant me(w.clock, "test");
  auto q = w.three_runs();
  Executor ex(w.context(), w.options());
  const auto began = std::chrono::steady_clock::now();
  const auto r = ex.execute(q);
  const double real = real_seconds_since(began);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:success", "12346:success", "12347:success"}));
  EXPECT_GE(w.clock.now() - kStart, 3 * 600s);
  EXPECT_LT(w.clock.now() - kStart, 3 * 700s);  // and no more than the queue asks for
  EXPECT_LT(real, 5.0);
  EXPECT_EQ(w.stall_reports(), std::vector<std::string>{});
}

TEST_F(ExecutorVirtual, RunTimestampsFollowTheClock) {
  World w({});
  Clock::Participant me(w.clock, "test");
  auto q = w.three_runs();
  Executor ex(w.context(), w.options());
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  const auto starts = w.starts();
  const auto stamps = w.persister.stamps();
  ASSERT_EQ(starts.size(), 3u);
  ASSERT_EQ(stamps.size(), 3u);
  EXPECT_EQ(starts[0].ts, kStart + 10s);  // the delay before the first run
  EXPECT_EQ(stamps[0], "2026-10-06T12:00:10Z");
  EXPECT_EQ(parse_utc(stamps[0]),
            std::chrono::floor<std::chrono::seconds>(
                kEpoch + std::chrono::duration_cast<WallTime::duration>(starts[0].ts - kStart)));
  EXPECT_GE(parse_utc(stamps[1]) - parse_utc(stamps[0]), 600s) << stamps[0] << " " << stamps[1];
  EXPECT_GE(parse_utc(stamps[2]) - parse_utc(stamps[1]), 600s) << stamps[1] << " " << stamps[2];
}

// The resource two overlapped runs contend for is the spectrometer: the
// second run's measurement begins at the instant the first run's ends.
TEST_F(ExecutorVirtual, OverlappedRunWaitsForTheResource) {
  World w({});
  Clock::Participant me(w.clock, "test");
  auto q = w.overlapped_runs();
  Executor ex(w.context(), w.options());
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:success", "12346:success"}));
  const auto first = w.run_id("12345"), second = w.run_id("12346");
  // The first run leaves its measurement, and with it the spectrometer.
  const auto released = w.entered(first, run::RunState::PostMeasuring);
  // The second has extracted and asks for the spectrometer.
  const auto asked = w.entered(second, run::RunState::Equilibrating);
  const auto got = w.first_block(second);
  ASSERT_TRUE(released && asked && got);
  EXPECT_LT(*asked + 200s, *released);  // it waited, and for long
  EXPECT_EQ(*got, *released);
  EXPECT_EQ(w.spec.overlapping.load(), 0);
  EXPECT_EQ(w.stall_reports(), std::vector<std::string>{});
}

// The thread that cancels is outside the clock, and the queue is asleep in a
// delay that is being paid for in real time.
TEST_F(ExecutorVirtual, CancelFromOutsideEndsAPacedQueue) {
  VirtualClock::Options o;
  o.speed = 1;
  World w(o);
  auto q = w.three_runs(Seconds{60});
  Executor ex(w.context(), w.options());
  QueueResult r;
  pychron::testing::Crew crew(w.clock);
  crew.start("queue", [&] { r = ex.execute(q); });
  // The queue is asleep in the delay before its first run, and stays there
  // for a minute of real time unless something wakes it.
  ASSERT_TRUE(pychron::testing::await_waiters(w.clock, 1));
  std::this_thread::sleep_for(50ms);
  const auto began = std::chrono::steady_clock::now();
  ex.cancel();
  crew.join();
  EXPECT_LT(real_seconds_since(began), 5.0);
  EXPECT_EQ(r.end, QueueEnd::Cancelled) << r.reason;
  EXPECT_TRUE(r.runs.empty());
  EXPECT_LT(w.clock.now() - kStart, 60s);  // cut short, not waited out
}

TEST_F(ExecutorVirtual, AbortWhileWaitingForAResource) {
  VirtualClock::Options o;
  o.stall_report_after = 5s;  // real: generous, the queue thread writes files
  World w(o);
  Clock::Participant me(w.clock, "test");
  auto q = w.overlapped_runs();
  Executor ex(w.context(), w.options());
  QueueResult r;
  pychron::testing::Crew crew(w.clock);
  crew.start("queue", [&] { r = ex.execute(q); });
  // By now the second run has extracted and waits for the spectrometer,
  // which the first keeps for minutes yet. The first is half way through a
  // reading, which nothing cuts short.
  w.clock.sleep_for(700s + 500ms);
  const auto at = w.clock.now();
  const auto first = w.run_id("12345"), second = w.run_id("12346");
  ASSERT_FALSE(second.empty());
  ASSERT_TRUE(w.entered(second, run::RunState::Equilibrating));
  ASSERT_FALSE(w.first_block(second));
  ASSERT_FALSE(w.entered(first, run::RunState::PostMeasuring));
  ex.abort();
  crew.join();
  EXPECT_EQ(r.end, QueueEnd::Aborted) << r.reason;
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:aborted", "12346:aborted"}));
  EXPECT_FALSE(w.first_block(second));  // it never got the spectrometer
  // The abort itself ended the second run's wait, not the first run letting
  // go of the spectrometer half a second later, when its reading was over.
  EXPECT_EQ(w.entered(second, run::RunState::Aborted), std::optional<TimePoint>(at));
  EXPECT_EQ(w.entered(first, run::RunState::Aborted), std::optional<TimePoint>(at + 500ms));
  EXPECT_EQ(w.clock.now(), at + 500ms);  // and nothing else was waited out
  EXPECT_EQ(w.stall_reports(), std::vector<std::string>{});
}

// The second run waits for the spectrometer, which the first keeps for ten
// minutes that are paid for in real time: only the cancel can end the wait.
// The thread that cancels is outside the clock.
TEST_F(ExecutorVirtual, CancelFromOutsideReachesARunWaitingForAResource) {
  World w({});
  std::atomic<bool> paced{false};
  // Well into the first run's main block, long after the second has
  // extracted: from here time is real, and the first run sits in a reading
  // that nothing cuts short.
  w.spec.on_reading = [&](int n) {
    if (n != 500) return;
    w.clock.set_speed(1);
    paced = true;
    w.clock.sleep_for(600s);
  };
  auto q = w.overlapped_runs();
  Executor ex(w.context(), w.options());
  QueueResult r;
  pychron::testing::Crew crew(w.clock);
  crew.start("queue", [&] { r = ex.execute(q); });
  // Asleep in the clock: the queue (for the second run), the first run (in
  // its reading) and the second (for the spectrometer).
  ASSERT_TRUE(pychron::testing::eventually_real([&] { return paced.load() && w.clock.waiters() == 3; }));
  const auto first = w.run_id("12345"), second = w.run_id("12346");
  ASSERT_TRUE(w.entered(second, run::RunState::Equilibrating));
  ASSERT_FALSE(w.first_block(second));
  const auto began = std::chrono::steady_clock::now();
  ex.cancel();
  // The second run is cancelled while the first still holds the spectrometer.
  EXPECT_TRUE(pychron::testing::eventually_real(
      [&] { return w.entered(second, run::RunState::Cancelled).has_value(); }));
  EXPECT_FALSE(w.entered(first, run::RunState::Cancelled));
  EXPECT_FALSE(w.entered(first, run::RunState::PostMeasuring));
  w.clock.set_speed(std::numeric_limits<double>::infinity());  // the first run's reading, at no cost
  crew.join();
  EXPECT_LT(real_seconds_since(began), 5.0);
  EXPECT_EQ(r.end, QueueEnd::Cancelled) << r.reason;
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:cancelled", "12346:cancelled"}));
  EXPECT_FALSE(w.first_block(second));  // it never got the spectrometer
}

// The post-equilibration script runs beside the measurement on a thread of
// its own, and the run does not go on until it has ended.
TEST_F(ExecutorVirtual, APostEquilibrationScriptThatOutlastsTheMeasurementIsWaitedFor) {
  World w({});
  Clock::Participant me(w.clock, "test");
  std::mutex mutex;
  std::optional<TimePoint> script_began, last_reading;
  w.host.bodies["post_eq"] = [&](const scripting::ScriptEnvironment& env, scripting::CancelToken& token) -> Result<void> {
    {
      std::lock_guard lock(mutex);
      script_began = env.clock->now();
    }
    token.wait_until(*env.clock, env.clock->now() + 1000s);
    return {};
  };
  w.spec.on_reading = [&](int) {
    std::lock_guard lock(mutex);
    last_reading = w.clock.now();
  };
  auto q = w.queue({unknown_run("12345")}, Seconds{0}, Seconds{0});
  Executor ex(w.context(), w.options());
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states_of(r), std::vector<std::string>{"12345:success"});
  ASSERT_TRUE(script_began && last_reading);
  const auto script_ended = *script_began + 1000s;
  EXPECT_LT(*last_reading + 900s, script_ended);  // the measurement was over long before
  // The run leaves its measurement when the script ends, and not before.
  EXPECT_EQ(w.entered(w.run_id("12345"), run::RunState::PostMeasuring), std::optional<TimePoint>(script_ended));
  EXPECT_EQ(w.clock.now(), script_ended);
  EXPECT_EQ(w.stall_reports(), std::vector<std::string>{});
}

// A stop asked for before execute() is not slept through: the queue does not
// wait for its scheduled start to find out it is not to run.
TEST_F(ExecutorVirtual, AStopBeforeExecuteDoesNotWaitForTheScheduledStart) {
  World w({});
  Clock::Participant me(w.clock, "test");
  auto q = w.queue({unknown_run("12345")}, Seconds{0}, Seconds{0});
  auto opts = w.options();
  opts.start_at = kStart + 8h;
  Executor ex(w.context(), opts);
  ex.stop();
  const auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Stopped) << r.reason;
  EXPECT_TRUE(r.runs.empty());
  EXPECT_EQ(w.clock.now(), kStart);
  EXPECT_EQ(w.stall_reports(), std::vector<std::string>{});
}

// The minimum pump time is counted from when the run before began to pump.
// That is said after it has let the spectrometer go, so the next run, which
// has the spectrometer by then, waits to hear it. Here the first run has no
// post-measurement script and says so at once; its thread is held between
// the two for as long as the second run would need to get ahead of it.
TEST_F(ExecutorVirtual, MinimumPumpTimeIsCountedFromThePreviousRunsPumping) {
  World w({});
  Clock::Participant me(w.clock, "test");
  w.host.bodies["extract"] = takes(300s);
  RunSpec first = counted_run("12345", 600);
  first.overlap.duration = Seconds{2};
  first.post_measurement.reset();
  RunSpec second = unknown_run("12346");
  second.overlap.min_delay = Seconds{120};
  auto q = w.queue({first, second}, Seconds{10}, Seconds{60});
  w.subs.push_back(w.bus.subscribe<run::RunStateChanged>([&](const run::RunStateChanged& e) {
    if (e.to != run::RunState::PostMeasuring || e.run_id != w.run_id("12345")) return;
    // The spectrometer is free and the pump time not yet said.
    (void)pychron::testing::eventually_real([&] { return w.first_block(w.run_id("12346")).has_value(); }, 200ms);
  }));
  Executor ex(w.context(), w.options());
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:success", "12346:success"}));
  const auto released = w.entered(w.run_id("12345"), run::RunState::PostMeasuring);
  const auto got = w.first_block(w.run_id("12346"));
  ASSERT_TRUE(released && got);
  EXPECT_EQ(*got, *released + 120s);
  EXPECT_EQ(w.spec.overlapping.load(), 0);
  EXPECT_EQ(w.stall_reports(), std::vector<std::string>{});
}

// With a post-measurement script the pumping begins when the script says so.
TEST_F(ExecutorVirtual, MinimumPumpTimeIsCountedFromWhenTheScriptSaysItPumps) {
  World w({});
  Clock::Participant me(w.clock, "test");
  w.host.bodies["extract"] = takes(300s);
  std::mutex mutex;
  std::optional<TimePoint> pumping;
  w.host.bodies["post_meas"] = [&](const scripting::ScriptEnvironment& env, scripting::CancelToken& token) -> Result<void> {
    token.wait_until(*env.clock, env.clock->now() + 50s);  // isolating, before the pump valve opens
    {
      std::lock_guard lock(mutex);
      if (!pumping) pumping = env.clock->now();
    }
    if (env.on_pump_time_start) env.on_pump_time_start();
    return {};
  };
  RunSpec first = counted_run("12345", 600);
  first.overlap.duration = Seconds{2};
  RunSpec second = unknown_run("12346");
  second.overlap.min_delay = Seconds{120};
  auto q = w.queue({first, second}, Seconds{10}, Seconds{60});
  Executor ex(w.context(), w.options());
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  const auto released = w.entered(w.run_id("12345"), run::RunState::PostMeasuring);
  const auto got = w.first_block(w.run_id("12346"));
  ASSERT_TRUE(released && got && pumping);
  EXPECT_EQ(*pumping, *released + 50s);
  EXPECT_EQ(*got, *pumping + 120s);
  EXPECT_EQ(w.spec.overlapping.load(), 0);
  EXPECT_EQ(w.stall_reports(), std::vector<std::string>{});
}

TEST_F(ExecutorVirtual, ScheduledStartWaitsOnTheClock) {
  World w({});
  Clock::Participant me(w.clock, "test");
  w.host.bodies["extract"] = takes(300s);
  auto q = w.queue({unknown_run("12345")}, Seconds{0}, Seconds{0});
  auto opts = w.options();
  opts.start_at = kStart + 8h;
  Executor ex(w.context(), opts);
  const auto began = std::chrono::steady_clock::now();
  const auto r = ex.execute(q);
  const double real = real_seconds_since(began);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  const auto starts = w.starts();
  ASSERT_EQ(starts.size(), 1u);
  EXPECT_EQ(starts[0].ts, kStart + 8h);
  EXPECT_LT(real, 5.0);
  EXPECT_EQ(w.stall_reports(), std::vector<std::string>{});
}

}  // namespace

// ---- Holding (executor hardening, spec 4.6) -----------------------------------

namespace {

// A link the runs need, on the world's clock, publishing its health.
struct Wire {
  explicit Wire(World& w) : clock(w.clock) {
    TransportOptions o;
    o.name = "valve_bus";
    o.clock = &w.clock;
    o.bus = &w.bus;
    transport = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, o);
    EXPECT_TRUE(transport->open());
    link = std::make_unique<Link>(*transport, w.clock);
    link->set_connect([this](Transport& t) -> Result<void> {
      {
        std::lock_guard lock(mutex);
        probes.push_back(clock.now());
      }
      return t.open();
    });
  }
  // Down for `d` from now (for good when not given), closed as a dropped link is.
  void down(std::optional<pychron::Duration> d = std::nullopt) {
    if (d) transport->down_for(*d);
    else transport->set_down(true);
    transport->close();
  }
  std::vector<TimePoint> probe_times() {
    std::lock_guard lock(mutex);
    return probes;
  }

  const Clock& clock;
  std::unique_ptr<SimTransport> transport;
  std::unique_ptr<Link> link;
  std::mutex mutex;
  std::vector<TimePoint> probes;
};

// What a held queue published.
struct HoldLog {
  explicit HoldLog(SignalBus& bus) {
    subs.push_back(bus.subscribe<ExecutorHolding>([this](const ExecutorHolding& h) {
      std::lock_guard lock(mutex);
      holds.push_back(h);
    }));
    subs.push_back(bus.subscribe<ExecutorStateChanged>([this](const ExecutorStateChanged& e) {
      std::lock_guard lock(mutex);
      states.push_back(e);
    }));
  }
  std::vector<ExecutorHolding> held() {
    std::lock_guard lock(mutex);
    return holds;
  }
  bool went(ExecutorState from, ExecutorState to) {
    std::lock_guard lock(mutex);
    return std::any_of(states.begin(), states.end(), [&](const auto& e) { return e.from == from && e.to == to; });
  }
  std::mutex mutex;
  std::vector<ExecutorHolding> holds;
  std::vector<ExecutorStateChanged> states;
  std::vector<SignalBus::Subscription> subs;
};

// A check that asks the queue to wait while `waiting` names the run's identifier.
class WaitFor final : public IPreRunCheck {
 public:
  std::string name() const override { return "test"; }
  std::optional<CheckFailure> check(const RunSpec& run) override {
    std::lock_guard lock(mutex);
    if (run.id.identifier != waiting) return std::nullopt;
    return CheckFailure{CheckKind::Wait, Error{ErrorKind::Io, "not yet", {}}, {}};
  }
  void set(std::string identifier) {
    std::lock_guard lock(mutex);
    waiting = std::move(identifier);
  }
  std::mutex mutex;
  std::string waiting;
};

// The world's context, needing the wire's link (checked and probed).
struct Held {
  explicit Held(World& w) : wire(w), links_up([this](const RunSpec&) { return LinkSet{{wire.link.get()}}; }), log(w.bus) {}
  ExecutorContext context(World& w) {
    auto c = w.context();
    c.checks = {&links_up};
    c.links = [this](const RunSpec&) { return std::vector<Link*>{wire.link.get()}; };
    return c;  // NOLINT(clang-analyzer-core.StackAddressEscape): by value; its pointers are into World and this
  }
  Wire wire;
  LinksUp links_up;
  HoldLog log;
};

int started_count(World& w, const std::string& identifier) {
  int n = 0;
  for (const auto& e : w.starts()) n += e.identifier == identifier;
  return n;
}
std::optional<TimePoint> started_at(World& w, const std::string& identifier, int nth = 0) {
  for (const auto& e : w.starts())
    if (e.identifier == identifier && nth-- == 0) return e.ts;
  return std::nullopt;
}

// After row 0 finishes, `act()` (the link drops, an operator acts...).
SignalBus::Subscription after_first_run(World& w, std::function<void()> act) {
  return w.bus.subscribe<RunFinished>([act = std::move(act)](const RunFinished& f) {
    if (f.summary.row == 0) act();
  });
}

}  // namespace

using ExecutorHolding_ = pychron::testing::VirtualTimeTest;
using pychron::testing::Crew;

namespace {

// An extraction that fails with `kind` the first `n` times, then succeeds.
FakeScriptHost::Body fails_first(int n, ErrorKind kind, const std::string& what) {
  auto left = std::make_shared<std::atomic<int>>(n);
  return [left, kind, what](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    if (left->fetch_sub(1) > 0) return fail(kind, what);
    return {};
  };
}

}  // namespace

TEST_F(ExecutorHolding_, ALinkDownBeforeARunHoldsThenStartsThatRunOnce) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  std::optional<TimePoint> dropped;
  auto sub = after_first_run(w, [&] {
    dropped = w.clock.now();
    h.wire.down(5min);
  });
  auto q = w.three_runs();
  Executor ex(h.context(w), w.options());
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:success", "12346:success", "12347:success"}));
  EXPECT_EQ(started_count(w, "12346"), 1);
  const auto held = h.log.held();
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held[0].row, std::optional<std::size_t>(1));
  EXPECT_EQ(held[0].links, std::vector<std::string>{"valve_bus"});
  EXPECT_EQ(held[0].until - held[0].since, 2h);
  EXPECT_TRUE(h.log.went(ExecutorState::Running, ExecutorState::Holding));
  EXPECT_TRUE(h.log.went(ExecutorState::Holding, ExecutorState::Running));
  // Back at 5 min, seen by a probe (every 15 s), then 30 s of settle.
  const auto second = *started_at(w, "12346");
  EXPECT_GE(second - *dropped, 5min + 30s);
  EXPECT_LE(second - *dropped, 5min + 15s + 30s + 2s);
}

TEST_F(ExecutorHolding_, WaitsForTheLinkToBeStableForSettle) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  auto options = w.options();
  options.hold.probe_interval = 1s;  // up is seen at once
  Crew crew(w.clock);
  std::optional<TimePoint> t0;
  auto sub = after_first_run(w, [&] {
    t0 = w.clock.now();
    h.wire.down();
    crew.start("outage", [&] {
      w.clock.sleep_for(60s);
      h.wire.transport->set_down(false);  // up at 60 s
      w.clock.sleep_for(10s);
      h.wire.down();  // down again at 70 s
      w.clock.sleep_for(30s);
      h.wire.transport->set_down(false);  // up at 100 s
    });
  });
  auto q = w.three_runs();
  Executor ex(h.context(w), options);
  const auto r = ex.execute(q);
  crew.join();
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  const auto second = *started_at(w, "12346") - *t0;
  EXPECT_GE(second, 130s);
  EXPECT_LE(second, 132s);
}

TEST_F(ExecutorHolding_, ProbesEachLinkAtTheProbeInterval) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  std::optional<TimePoint> t0;
  auto sub = after_first_run(w, [&] {
    t0 = w.clock.now();
    h.wire.down(130s);  // the hold begins after the 60 s delay between runs
  });
  auto q = w.three_runs();
  Executor ex(h.context(w), w.options());
  ASSERT_EQ(ex.execute(q).end, QueueEnd::Completed);
  // The probes of the hold (a probe may reconnect once more within it).
  std::vector<TimePoint> starts;
  for (const auto& t : h.wire.probe_times())
    if (t >= *t0 && (starts.empty() || t - starts.back() > 5s)) starts.push_back(t);
  ASSERT_GE(starts.size(), 4u);
  for (std::size_t i = 1; i < starts.size(); ++i) EXPECT_EQ(starts[i] - starts[i - 1], 15s) << i;
}

TEST_F(ExecutorHolding_, ARunThatFailedForALinkHoldsThenGoesOnToTheNextRow) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  auto q = w.three_runs();
  w.host.bodies["extract"] = fails_first(1, ErrorKind::Io, "laser link down");
  Executor ex(h.context(w), w.options());
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:failed", "12346:success", "12347:success"}));
  EXPECT_EQ(started_count(w, "12345"), 1);  // past its point of no return: never again
  const auto held = h.log.held();
  ASSERT_EQ(held.size(), 1u);
  EXPECT_FALSE(held[0].row);  // held after a run that started
  EXPECT_NE(held[0].reason.find("laser link down"), std::string::npos) << held[0].reason;
  EXPECT_EQ(r.runs[0].status, record::RecordStatus::Failed);  // flagged in its summary
  EXPECT_EQ(r.runs[0].failure, "laser link down");
  // Made safe before the hold (the run did it as it failed; spec 4.7).
  EXPECT_NE(std::find(w.valves.log.begin(), w.valves.log.end(), "close B"), w.valves.log.end());
}

TEST_F(ExecutorHolding_, AfterLinkFailureEndEndsTheQueue) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  auto q = w.three_runs();
  w.host.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "laser link down");
  };
  auto options = w.options();
  options.hold.end_after_link_failure = true;
  Executor ex(h.context(w), options);
  const auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Failed);
  EXPECT_EQ(r.runs.size(), 1u);
  EXPECT_TRUE(h.log.held().empty());
}

TEST_F(ExecutorHolding_, TheHoldLimitEndsTheQueueAsFailedNamingTheLinks) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  std::optional<TimePoint> dropped;
  auto sub = after_first_run(w, [&] {
    dropped = w.clock.now();
    h.wire.down();
  });
  auto q = w.three_runs();
  Executor ex(h.context(w), w.options());
  const auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Failed);
  EXPECT_NE(r.reason.find("hold limit"), std::string::npos) << r.reason;
  EXPECT_NE(r.reason.find("valve_bus"), std::string::npos) << r.reason;
  EXPECT_EQ(r.runs.size(), 1u);
  EXPECT_GE(w.clock.now() - *dropped, 2h);
}

TEST_F(ExecutorHolding_, ResumeSkipsSettle) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  auto options = w.options();
  options.hold.probe_interval = 1s;
  std::optional<TimePoint> back;
  Executor* exec = nullptr;
  auto health = w.bus.subscribe<TransportHealth>([&](const TransportHealth& e) {
    if (e.connected && back == std::nullopt && exec != nullptr && exec->state() == ExecutorState::Holding) {
      back = w.clock.now();
      exec->resume();
    }
  });
  auto sub = after_first_run(w, [&] { h.wire.down(60s); });
  auto q = w.three_runs();
  Executor ex(h.context(w), options);
  exec = &ex;
  ASSERT_EQ(ex.execute(q).end, QueueEnd::Completed);
  ASSERT_TRUE(back);
  EXPECT_LT(*started_at(w, "12346") - *back, 5s);  // not the 30 s of settle
}

TEST_F(ExecutorHolding_, SkipDropsTheHeldRow) {
  World w({});
  Clock::Participant me(w.clock, "test");
  HoldLog log(w.bus);
  WaitFor check;
  check.set("12346");
  Executor* exec = nullptr;
  std::optional<Result<void>> skipped;
  auto sub = w.bus.subscribe<ExecutorHolding>([&](const ExecutorHolding&) { skipped = exec->skip(); });
  auto ctx = w.context();
  ctx.checks = {&check};
  auto q = w.three_runs();
  Executor ex(ctx, w.options());
  exec = &ex;
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  ASSERT_TRUE(skipped && *skipped);
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:success", "12347:success"}));
  EXPECT_EQ(started_count(w, "12346"), 0);
}

TEST_F(ExecutorHolding_, SkipIsRefusedForARowThatStarted) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  Executor* exec = nullptr;
  std::optional<Result<void>> skipped;
  auto sub = w.bus.subscribe<ExecutorHolding>([&](const ExecutorHolding&) { skipped = exec->skip(); });
  auto q = w.three_runs();
  w.host.bodies["extract"] = fails_first(1, ErrorKind::Io, "laser link down");
  Executor ex(h.context(w), w.options());
  exec = &ex;
  const auto r = ex.execute(q);
  ASSERT_TRUE(skipped);
  ASSERT_FALSE(*skipped);
  EXPECT_NE(skipped->error().what.find("already started"), std::string::npos);
  EXPECT_EQ(r.runs.size(), 3u);
}

TEST_F(ExecutorHolding_, StopCancelAndAbortLeaveAHold) {
  for (const auto how : {QueueEnd::Stopped, QueueEnd::Cancelled, QueueEnd::Aborted}) {
    World w({});
    Clock::Participant me(w.clock, "test");
    Held h(w);
    Executor* exec = nullptr;
    auto sub = w.bus.subscribe<ExecutorHolding>([&](const ExecutorHolding&) {
      if (how == QueueEnd::Stopped) exec->stop();
      if (how == QueueEnd::Cancelled) exec->cancel();
      if (how == QueueEnd::Aborted) exec->abort();
    });
    auto drop = after_first_run(w, [&] { h.wire.down(); });
    auto q = w.three_runs();
    Executor ex(h.context(w), w.options());
    exec = &ex;
    const TimePoint began = w.clock.now();
    const auto r = ex.execute(q);
    EXPECT_EQ(r.end, how) << r.reason;
    EXPECT_EQ(r.runs.size(), 1u);
    EXPECT_LT(w.clock.now() - began, 1h);  // not the hold limit
  }
}

TEST_F(ExecutorHolding_, AnUnsafeRunEndsTheQueueWithoutHolding) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  auto q = w.three_runs();
  w.host.bodies["extract"] = [](const scripting::ScriptEnvironment&, scripting::CancelToken&) -> Result<void> {
    return fail(ErrorKind::Io, "laser link down");
  };
  // The safe state cannot close the inlet: it is interlocked (definite).
  w.valves.fault = [](const std::string& v, bool open) -> Result<void> {
    if (!open && v == "B") return fail(ErrorKind::Interlock, "B is interlocked");
    return {};
  };
  Executor ex(h.context(w), w.options());
  const auto r = ex.execute(q);
  EXPECT_EQ(r.end, QueueEnd::Failed);
  EXPECT_EQ(r.code, kUnsafe);
  EXPECT_NE(r.reason.find("close inlet B"), std::string::npos) << r.reason;
  EXPECT_TRUE(h.log.held().empty());
}

TEST_F(ExecutorHolding_, ADefiniteFailureEndsTheQueueAsToday) {
  for (const bool go_on : {false, true}) {
    World w({});
    Clock::Participant me(w.clock, "test");
    Held h(w);
    auto q = w.three_runs();
    w.host.bodies["extract"] = fails_first(1, ErrorKind::Protocol, "laser fault");
    auto options = w.options();
    options.continue_on_failure = go_on;
    Executor ex(h.context(w), options);
    const auto r = ex.execute(q);
    EXPECT_TRUE(h.log.held().empty());
    if (go_on) {
      EXPECT_EQ(r.end, QueueEnd::Completed) << r.reason;
      EXPECT_EQ(r.runs.size(), 3u);
    } else {
      EXPECT_EQ(r.end, QueueEnd::Failed);
      EXPECT_EQ(r.runs.size(), 1u);
    }
  }
}

TEST_F(ExecutorHolding_, ALinkThatDropsDuringPrepareHoldsAndStartsTheRowAgain) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  w.persister.fail_begins = 1;  // the first run's prepare
  std::optional<std::size_t> next_row_while_held;
  Executor* exec = nullptr;
  auto sub = w.bus.subscribe<ExecutorHolding>([&](const ExecutorHolding&) {
    auto row = Executor::resume_row(w.lab.dir() / "executor_state.json");
    if (row) next_row_while_held = *row;
    (void)exec;
  });
  auto q = w.three_runs();
  Executor ex(h.context(w), w.options());
  exec = &ex;
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  ASSERT_EQ(r.runs.size(), 4u);  // the attempt, then the run
  EXPECT_EQ(r.runs[0].state, run::RunState::Failed);
  EXPECT_EQ(r.runs[1].identifier, "12345");
  EXPECT_EQ(r.runs[1].state, run::RunState::Success);
  EXPECT_EQ(r.runs[1].aliquot, r.runs[0].aliquot);  // allocated once
  EXPECT_EQ(next_row_while_held, std::optional<std::size_t>(0));  // not consumed
  const auto held = h.log.held();
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held[0].row, std::optional<std::size_t>(0));
  // A definite failure in prepare ends the queue as before.
  World w2({});
  Clock::Participant me2(w2.clock, "test2");
  auto bad = w2.queue({unknown_run("12345", "no_such_plan")}, Seconds{0}, Seconds{0});
  Executor ex2(w2.context(), w2.options());
  EXPECT_EQ(ex2.execute(bad).end, QueueEnd::Failed);
}

TEST_F(ExecutorHolding_, TheStateFileCarriesTheHoldAndTheLastFailedRow) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  std::string while_held;
  auto sub = w.bus.subscribe<ExecutorHolding>([&](const ExecutorHolding&) {
    std::ifstream in(w.lab.dir() / "executor_state.json");
    while_held.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  });
  auto q = w.three_runs();
  w.host.bodies["extract"] = fails_first(1, ErrorKind::Io, "laser link down");
  Executor ex(h.context(w), w.options());
  ASSERT_EQ(ex.execute(q).end, QueueEnd::Completed);
  EXPECT_NE(while_held.find("\"holding\": {\"reason\": "), std::string::npos) << while_held;
  EXPECT_NE(while_held.find("\"last_failed_row\": 0"), std::string::npos) << while_held;
  // A file with the hold gives the same resume_row as one without.
  const auto with = w.lab.dir() / "with.json";
  const auto without = w.lab.dir() / "without.json";
  std::ofstream(with) << while_held;
  std::ofstream(without) << "{\n  \"queue\": \"q1\",\n  \"state\": \"running\",\n  \"next_row\": 1,\n  \"runs\": []\n}\n";
  EXPECT_EQ(Executor::resume_row(with).value(), 1u);
  EXPECT_EQ(Executor::resume_row(without).value(), 1u);
}

TEST_F(ExecutorHolding_, InOverlapTheRunInFlightCarriesOn) {
  World w({});
  Clock::Participant me(w.clock, "test");
  Held h(w);
  std::atomic<bool> once{false};
  auto drop = w.bus.subscribe<measurement::OverlapReady>([&](const measurement::OverlapReady&) {
    if (!once.exchange(true)) h.wire.down(5min);
  });
  auto q = w.overlapped_runs();
  Executor ex(h.context(w), w.options());
  const auto r = ex.execute(q);
  ASSERT_EQ(r.end, QueueEnd::Completed) << r.reason;
  EXPECT_EQ(states_of(r), (std::vector<std::string>{"12345:success", "12346:success"}));
  ASSERT_EQ(h.log.held().size(), 1u);
  EXPECT_EQ(h.log.held()[0].row, std::optional<std::size_t>(1));
}
