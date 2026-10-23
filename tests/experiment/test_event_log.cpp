#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "log_capture.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/collect/collector.hpp"
#include "pychron/experiment/event_log.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/notifier.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/run/run.hpp"
#include "pychron/experiment/run/state.hpp"

namespace pychron::experiment {
namespace {

using pychron::testing::LogCapture;
using Lines = std::vector<std::string>;
using executor::ExecutorState;
using run::RunState;

// Every struct libs/experiment publishes on a SignalBus: executor/executor.hpp,
// run/state.hpp, run/run.hpp, lab/session.hpp, lab/notifier.hpp,
// measurement/engine.hpp, collect/collector.hpp, persist/persister.hpp. Kept
// by hand: a new event fails EveryEventClassified until
// experiment_event_classes() has it.
const std::set<std::string_view> kExperimentEvents{
    "ExecutorStateChanged", "RunStarted",     "RunFinished",        "QueueEdited",   "QueueFrontier",
    "ExecutorWaiting",      "ExecutorHolding", "RunStateChanged",   "RunNote",       "QueueStarted",
    "QueueEnded",           "BlockStarted",   "BlockFinished",      "MeasurementGap", "ConditionalTripped",
    "CountsProgress",       "OverlapReady",   "NotificationSent",   "NotificationOutbox", "SeriesUpdated",
    "FitsUpdated",          "SpoolStatus",
};

TEST(ExperimentEventLog, EveryEventClassified) {
  std::set<std::string_view> classified;
  for (const auto& c : experiment_event_classes()) {
    EXPECT_TRUE(classified.insert(c.name).second) << c.name << " twice";
  }
  EXPECT_EQ(classified, kExperimentEvents);
}

class ExperimentEventLogTest : public ::testing::Test {
 protected:
  LogCapture log;
  SignalBus bus;
  ExperimentEventRecorder recorder{bus};
};

TEST_F(ExperimentEventLogTest, ExecutorStateChanged) {
  ASSERT_TRUE(log.ok());
  bus.publish(executor::ExecutorStateChanged{ExecutorState::Running, ExecutorState::Finalizing, "queue done"});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.ExecutorStateChanged: from=running, to=finalizing, reason=queue done"}));
}

TEST_F(ExperimentEventLogTest, RunStarted) {
  bus.publish(executor::RunStarted{2, "12345-01A", "12345", {}});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.RunStarted: row=2, run_id=12345-01A, identifier=12345"}));
}

TEST_F(ExperimentEventLogTest, RunFinishedIsInfoWhenTheRunFailed) {
  executor::RunSummary s;
  s.row = 2;
  s.identifier = "12345";
  s.run_id = "12345-01A";
  s.aliquot = 1;
  s.step = "A";
  s.state = RunState::Success;
  s.messages = {"said"};
  bus.publish(executor::RunFinished{s});
  s.state = RunState::Failed;
  s.status = record::RecordStatus::Failed;
  s.failure = "link";
  s.error = "spectrometer: timeout";
  bus.publish(executor::RunFinished{s});
  s.state = RunState::Success;
  s.status = record::RecordStatus::Complete;
  s.failure.clear();
  s.error.reset();
  s.save_error = true;
  bus.publish(executor::RunFinished{s});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.RunFinished: row=2, identifier=12345, run_id=12345-01A, aliquot=1, step=A, "
                   "state=success, truncated=false, save_error=false, status=complete",
                   "[info] trace.RunFinished: row=2, identifier=12345, run_id=12345-01A, aliquot=1, step=A, "
                   "state=failed, truncated=false, save_error=false, status=failed, failure=link, "
                   "error=spectrometer: timeout",
                   "[info] trace.RunFinished: row=2, identifier=12345, run_id=12345-01A, aliquot=1, step=A, "
                   "state=success, truncated=false, save_error=true, status=complete"}));
}

TEST_F(ExperimentEventLogTest, QueueEditedAndFrontier) {
  QueueSpec q;
  q.runs.resize(4);
  bus.publish(executor::QueueEdited{q, {"insert blank", "skip 3"}, 5, 2});
  bus.publish(executor::QueueFrontier{3, 5});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.QueueEdited: rows=4, changes=[insert blank,skip 3], version=5, frozen=2",
                   "[debug] trace.QueueFrontier: frozen=3, version=5"}));
}

TEST_F(ExperimentEventLogTest, ExecutorWaitingAndHolding) {
  bus.publish(executor::ExecutorWaiting{"delay between analyses", std::chrono::seconds(30), {}, ""});
  bus.publish(executor::ExecutorHolding{"link down", {"ngx"}, {}, {}, 4, executor::HoldReason::Link});
  bus.publish(executor::ExecutorHolding{"spool full", {}, {}, {}, std::nullopt, executor::HoldReason::Spool});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.ExecutorWaiting: reason=delay between analyses, duration=30s, run_id=",
                   "[debug] trace.ExecutorHolding: reason=link down, links=[ngx], row=4, kind=link",
                   "[debug] trace.ExecutorHolding: reason=spool full, links=[], kind=spool"}));
}

TEST_F(ExperimentEventLogTest, RunStateChangedAndNote) {
  bus.publish(run::RunStateChanged{"12345-01A", RunState::Measuring, RunState::Truncated, "conditional", {}});
  bus.publish(run::RunNote{"12345-01A", 2, "hole 3: centered", {}});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.RunStateChanged: run_id=12345-01A, from=measuring, to=truncated, reason=conditional",
                   "[info] trace.RunNote: run_id=12345-01A, row=2, text=hole 3: centered"}));
}

TEST_F(ExperimentEventLogTest, QueueStartedAndEnded) {
  bus.publish(lab::QueueStarted{6, 1});
  executor::QueueResult r;
  r.end = executor::QueueEnd::Failed;
  r.reason = "run failed";
  r.runs.resize(2);
  r.code = executor::kUnsafe;
  bus.publish(lab::QueueEnded{r});
  EXPECT_EQ(log.trace_lines(), (Lines{"[info] trace.QueueStarted: rows=6, from_row=1",
                                      "[info] trace.QueueEnded: end=failed, reason=run failed, runs=2, code=unsafe"}));
}

TEST_F(ExperimentEventLogTest, Blocks) {
  bus.publish(measurement::BlockStarted{"r1", measurement::Block::BaselineBefore, "", -1});
  bus.publish(measurement::BlockFinished{"r1", measurement::Block::Main, false, "Ar", 2});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.BlockStarted: run_id=r1, block=baseline.before, gas=, segment=-1",
                   "[debug] trace.BlockFinished: run_id=r1, block=main, ok=false, gas=Ar, segment=2"}));
}

TEST_F(ExperimentEventLogTest, MeasurementGapWarnsWhenTheReadingsDidNotResume) {
  bus.publish(measurement::MeasurementGap{"r1", measurement::Block::Main, 30, 51, true, {}});
  bus.publish(measurement::MeasurementGap{"r1", measurement::Block::Main, 60, 90.5, false, {}});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[info] trace.MeasurementGap: run_id=r1, block=main, from=30, to=51, resumed=true",
                   "[warn] trace.MeasurementGap: run_id=r1, block=main, from=60, to=90.5, resumed=false"}));
}

TEST_F(ExperimentEventLogTest, ConditionalTripped) {
  Trip trip;
  trip.name = "max_ar40";
  trip.kind = ConditionalKind::Truncation;
  trip.value = 12.5;
  trip.count = 3;
  trip.reading = 40;
  trip.check = "Ar40 > 10";
  bus.publish(measurement::ConditionalTripped{"r1", trip});
  EXPECT_EQ(log.trace_lines(), (Lines{"[info] trace.ConditionalTripped: run_id=r1, name=max_ar40, kind=truncation, "
                                      "value=12.5, count=3, reading=40, check=Ar40 > 10"}));
}

TEST_F(ExperimentEventLogTest, NotificationSent) {
  bus.publish(lab::NotificationSent{"email", lab::NotifyEvent::RunFailed, "run failed", false, "refused"});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.NotificationSent: channel=email, event=run_failed, "
                                      "subject=run failed, ok=false, error=refused"}));
}

TEST_F(ExperimentEventLogTest, SpoolStatusWarnsWithAnError) {
  bus.publish(persist::SpoolStatus{2, std::nullopt});
  bus.publish(persist::SpoolStatus{2, std::string("abc: refused")});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.SpoolStatus: pending=2",
                                      "[warn] trace.SpoolStatus: pending=2, last_error=abc: refused"}));
}

TEST_F(ExperimentEventLogTest, NeverLoggedEventsWriteNothing) {
  const auto before = log.lines();
  bus.publish(measurement::CountsProgress{"r1", measurement::Block::Main, 1, 10});
  bus.publish(measurement::OverlapReady{"r1"});
  bus.publish(lab::NotificationOutbox{3});
  bus.publish(collect::SeriesUpdated{});
  bus.publish(collect::FitsUpdated{});
  EXPECT_EQ(log.lines(), before);
}

}  // namespace
}  // namespace pychron::experiment
