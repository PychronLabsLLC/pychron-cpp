// ExperimentMetrics against events published by hand: what each event of a
// queue's life does to the registry.
#include "pychron/experiment/metrics/experiment_metrics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#include "metrics_text.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/notifier.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/run/state.hpp"
#include "pychron/metrics/registry.hpp"
#include "pychron/systems/jobs/job.hpp"

using namespace pychron;
using namespace pychron::experiment;
using executor::ExecutorState;
using executor::QueueEnd;
using metrics_text::has;
using metrics_text::value;
using run::RunState;

namespace {

TimePoint at(int seconds) { return TimePoint(std::chrono::seconds(seconds)); }

// The registry and the bus come first: the object under test points at both.
struct ExperimentMetricsTest : ::testing::Test {
  pychron::metrics::Registry registry;
  SignalBus bus;
  double now = 5000.0;  // seconds on a clock that only moves forward
  std::unique_ptr<experiment::metrics::ExperimentMetrics> metrics =
      std::make_unique<experiment::metrics::ExperimentMetrics>(registry, bus, [this] { return now; });

  std::string text() { return registry.render(); }

  void state(const std::string& run_id, RunState from, RunState to, int seconds) {
    bus.publish(run::RunStateChanged{run_id, from, to, "", at(seconds)});
  }
  void finished(RunState s, bool truncated = false, bool save_error = false, std::string identifier = "12345-01A") {
    executor::RunSummary sum;
    sum.identifier = std::move(identifier);
    sum.run_id = "run-" + sum.identifier;
    sum.state = s;
    sum.truncated = truncated;
    sum.save_error = save_error;
    bus.publish(executor::RunFinished{sum});
  }
  void ended(QueueEnd end) { bus.publish(lab::QueueEnded{executor::QueueResult{end, "", {}}}); }

  // A whole run, r, from Pending to `last`, one state every 10 s from `t0`.
  void walk_to_measuring(const std::string& r, int t0) {
    state(r, RunState::Pending, RunState::Preparing, t0);
    state(r, RunState::Preparing, RunState::Extracting, t0 + 10);
    state(r, RunState::Extracting, RunState::Equilibrating, t0 + 20);
    state(r, RunState::Equilibrating, RunState::Measuring, t0 + 30);
  }
};

const char* const kDuration = "pychron_run_state_duration_seconds";

}  // namespace

TEST_F(ExperimentMetricsTest, BeforeAnyQueueTheExecutorIsIdle) {
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_executor_state{state=\"idle\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_executor_state{state=\"running\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_queue_active"), 0.0);
}

TEST_F(ExperimentMetricsTest, ExecutorStateIsOneOfSeven) {
  bus.publish(executor::ExecutorStateChanged{ExecutorState::Idle, ExecutorState::Running, ""});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_executor_state{state=\"running\"}"), 1.0);
  for (const char* other : {"idle", "preparing", "stopping", "cancelling", "aborting", "finalizing"}) {
    EXPECT_DOUBLE_EQ(value(t, std::string("pychron_executor_state{state=\"") + other + "\"}"), 0.0) << other;
  }
}

TEST_F(ExperimentMetricsTest, QueueActiveFollowsTheQueue) {
  bus.publish(lab::QueueStarted{4, 0});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_active"), 1.0);
  ended(QueueEnd::Completed);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_active"), 0.0);
}

TEST_F(ExperimentMetricsTest, QueueActiveAlsoSetByTheFirstBusyState) {
  // An executor driven without a session publishes no QueueStarted.
  bus.publish(executor::ExecutorStateChanged{ExecutorState::Idle, ExecutorState::Preparing, ""});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_active"), 1.0);
  bus.publish(executor::ExecutorStateChanged{ExecutorState::Finalizing, ExecutorState::Idle, ""});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_active"), 0.0);
}

TEST_F(ExperimentMetricsTest, QueueProgress) {
  bus.publish(lab::QueueStarted{10, 2});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_runs{status=\"total\"}"), 8.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_runs{status=\"done\"}"), 0.0);
  finished(RunState::Success);
  finished(RunState::Failed);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_runs{status=\"done\"}"), 2.0);

  executor::QueueEdited edited;
  edited.queue.runs.resize(12);
  edited.frozen = 4;
  bus.publish(edited);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_runs{status=\"total\"}"), 10.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_runs{status=\"done\"}"), 2.0);
}

TEST_F(ExperimentMetricsTest, RunsByOutcome) {
  for (int i = 0; i < 6; ++i) bus.publish(executor::RunStarted{static_cast<std::size_t>(i), "r", "12345-01A", at(i)});
  finished(RunState::Success);
  finished(RunState::Success, true);
  finished(RunState::Failed);
  finished(RunState::Failed, false, true);
  finished(RunState::Cancelled);
  finished(RunState::Aborted);
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_runs_started_total"), 6.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_runs_finished_total{state=\"success\",truncated=\"false\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_runs_finished_total{state=\"success\",truncated=\"true\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_runs_finished_total{state=\"failed\",truncated=\"false\"}"), 2.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_runs_finished_total{state=\"cancelled\",truncated=\"false\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_runs_finished_total{state=\"aborted\",truncated=\"false\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_run_save_errors_total"), 1.0);
}

TEST_F(ExperimentMetricsTest, CountersReadZeroBeforeTheFirstRun) {
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_runs_started_total"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_run_save_errors_total"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_runs_finished_total{state=\"failed\",truncated=\"false\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_queues_ended_total{end=\"failed\"}"), 0.0);
  // The rare ones above all: the first trip, the first failed block, the
  // first wait must each show as an increase.
  EXPECT_DOUBLE_EQ(value(t, "pychron_conditional_trips_total{kind=\"termination\",level=\"system\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_conditional_trips_total{kind=\"pre_run\",level=\"hook\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_measurement_blocks_total{block=\"peak_center_before\",ok=\"false\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_measurement_blocks_total{block=\"main\",ok=\"true\"}"), 0.0);
  for (const char* reason : {"scheduled_start", "delay", "extraction_device", "pump_time", "other"}) {
    EXPECT_DOUBLE_EQ(value(t, std::string("pychron_executor_waits_total{reason=\"") + reason + "\"}"), 0.0) << reason;
  }
}

// A channel is known only once it has been used; from then every event and outcome counts from zero.
TEST_F(ExperimentMetricsTest, AChannelsOtherOutcomesReadZero) {
  bus.publish(lab::NotificationSent{"email", lab::NotifyEvent::RunFailed, "s", true, ""});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_notifications_total{channel=\"email\",event=\"run_failed\",ok=\"false\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_notifications_total{channel=\"email\",event=\"queue_ended\",ok=\"false\"}"), 0.0);
}

TEST_F(ExperimentMetricsTest, StateDurations) {
  state("r1", RunState::Pending, RunState::Preparing, 0);
  state("r1", RunState::Preparing, RunState::Extracting, 4);
  state("r1", RunState::Extracting, RunState::Equilibrating, 64);
  state("r1", RunState::Equilibrating, RunState::Measuring, 84);
  state("r1", RunState::Measuring, RunState::PostMeasuring, 684);
  state("r1", RunState::PostMeasuring, RunState::Saving, 700);
  state("r1", RunState::Saving, RunState::Success, 702);
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_sum{state=\"preparing\"}"), 4.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_sum{state=\"extracting\"}"), 60.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_sum{state=\"equilibrating\"}"), 20.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_sum{state=\"measuring\"}"), 600.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_sum{state=\"post_measuring\"}"), 16.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_sum{state=\"saving\"}"), 2.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_count{state=\"extracting\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_bucket{state=\"extracting\",le=\"60\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_bucket{state=\"extracting\",le=\"30\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_bucket{state=\"measuring\",le=\"7200\"}"), 1.0);
  // Pending is the wait in the queue, not a phase of the run.
  EXPECT_FALSE(has(t, std::string(kDuration) + "_count{state=\"pending\"}"));
}

TEST_F(ExperimentMetricsTest, OverlappingRunsAreTimedSeparately) {
  state("r1", RunState::Pending, RunState::Preparing, 0);
  state("r1", RunState::Preparing, RunState::Extracting, 10);
  state("r2", RunState::Pending, RunState::Preparing, 15);
  state("r1", RunState::Extracting, RunState::Equilibrating, 40);  // r1 extracted for 30 s
  state("r2", RunState::Preparing, RunState::Extracting, 20);
  state("r2", RunState::Extracting, RunState::Equilibrating, 70);  // r2 for 50 s
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_sum{state=\"extracting\"}"), 80.0);
  EXPECT_DOUBLE_EQ(value(t, std::string(kDuration) + "_count{state=\"extracting\"}"), 2.0);
}

TEST_F(ExperimentMetricsTest, AFinishedRunIsForgotten) {
  for (const RunState last : {RunState::Failed, RunState::Cancelled, RunState::Aborted}) {
    walk_to_measuring("r", 0);
    EXPECT_EQ(metrics->tracked_runs(), 1u);
    state("r", RunState::Measuring, last, 100);
    EXPECT_EQ(metrics->tracked_runs(), 0u) << to_string(last);
  }
  // The time in the state a run ended in is still counted.
  EXPECT_DOUBLE_EQ(value(text(), std::string(kDuration) + "_count{state=\"measuring\"}"), 3.0);
}

TEST_F(ExperimentMetricsTest, AnAbortedQueueLeavesNothingBehind) {
  bus.publish(lab::QueueStarted{5, 0});
  walk_to_measuring("r1", 0);
  finished(RunState::Success);
  ended(QueueEnd::Aborted);  // r1 never said it ended
  EXPECT_EQ(metrics->tracked_runs(), 0u);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queue_active"), 0.0);
  EXPECT_DOUBLE_EQ(value(text(), "pychron_queues_ended_total{end=\"aborted\"}"), 1.0);

  bus.publish(lab::QueueStarted{3, 0});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_queue_runs{status=\"total\"}"), 3.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_queue_runs{status=\"done\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_queue_active"), 1.0);
}

TEST_F(ExperimentMetricsTest, AClockThatGoesBackwardsCountsNoTime) {
  state("r1", RunState::Pending, RunState::Preparing, 100);
  state("r1", RunState::Preparing, RunState::Extracting, 90);
  EXPECT_DOUBLE_EQ(value(text(), std::string(kDuration) + "_sum{state=\"preparing\"}"), 0.0);
}

TEST_F(ExperimentMetricsTest, MeasurementBlocks) {
  bus.publish(measurement::BlockFinished{"r1", measurement::Block::Main, true});
  bus.publish(measurement::BlockFinished{"r1", measurement::Block::BaselineAfter, false});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_measurement_blocks_total{block=\"main\",ok=\"true\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_measurement_blocks_total{block=\"baseline_after\",ok=\"false\"}"), 1.0);
}

TEST_F(ExperimentMetricsTest, ConditionalTrips) {
  Trip trip;
  trip.name = "age > 30 for 12345-01A";
  trip.kind = ConditionalKind::Truncation;
  trip.level = ConditionalLevel::Queue;
  bus.publish(measurement::ConditionalTripped{"r1", trip});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_conditional_trips_total{kind=\"truncation\",level=\"queue\"}"), 1.0);
  EXPECT_EQ(t.find("age > 30"), std::string::npos);
}

TEST_F(ExperimentMetricsTest, WaitReasonsAreAFixedSet) {
  for (const char* reason :
       {"scheduled start", "delay before 12345-01A", "extraction device", "minimum pump time", "something new"}) {
    bus.publish(executor::ExecutorWaiting{reason, {}, at(0), ""});
  }
  const std::string t = text();
  for (const char* label : {"scheduled_start", "delay", "extraction_device", "pump_time", "other"}) {
    EXPECT_DOUBLE_EQ(value(t, std::string("pychron_executor_waits_total{reason=\"") + label + "\"}"), 1.0) << label;
  }
  EXPECT_EQ(t.find("12345-01A"), std::string::npos);
  EXPECT_EQ(t.find("something new"), std::string::npos);
}

TEST_F(ExperimentMetricsTest, Notifications) {
  bus.publish(lab::NotificationSent{"email", lab::NotifyEvent::RunFailed, "Run 12345-01A failed", false, "550 refused"});
  bus.publish(lab::NotificationSent{"email", lab::NotifyEvent::QueueEnded, "Queue ended", true, ""});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_notifications_total{channel=\"email\",event=\"run_failed\",ok=\"false\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_notifications_total{channel=\"email\",event=\"queue_ended\",ok=\"true\"}"), 1.0);
  EXPECT_EQ(t.find("550"), std::string::npos);
  EXPECT_EQ(t.find("12345-01A"), std::string::npos);
}

TEST_F(ExperimentMetricsTest, ANotificationWithNoChannelIsChannelNone) {
  bus.publish(lab::NotificationSent{"", lab::NotifyEvent::RunFailed, "s", false, "no notifications are configured"});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_notifications_total{channel=\"none\",event=\"run_failed\",ok=\"false\"}"), 1.0);
}

TEST_F(ExperimentMetricsTest, TheAgeOfTheLastRunIsRealTimeSinceItFinished) {
  EXPECT_FALSE(has(text(), "pychron_last_run_finished_age_seconds")) << "no run has finished yet";
  finished(RunState::Success);
  now += 90.0;
  EXPECT_DOUBLE_EQ(value(text(), "pychron_last_run_finished_age_seconds"), 90.0);
  finished(RunState::Failed);
  now += 5.0;
  EXPECT_DOUBLE_EQ(value(text(), "pychron_last_run_finished_age_seconds"), 5.0);
}

TEST_F(ExperimentMetricsTest, RunIdsAndIdentifiersAreNeverRendered) {
  bus.publish(lab::QueueStarted{2, 0});
  bus.publish(executor::RunStarted{0, "run-77777-09B", "77777-09B", at(0)});
  walk_to_measuring("run-77777-09B", 0);
  bus.publish(measurement::BlockFinished{"run-77777-09B", measurement::Block::Main, true});
  bus.publish(executor::ExecutorWaiting{"minimum pump time", {}, at(0), "run-77777-09B"});
  state("run-77777-09B", RunState::Measuring, RunState::Failed, 50);
  finished(RunState::Failed, false, false, "77777-09B");
  ended(QueueEnd::Failed);
  EXPECT_EQ(text().find("77777"), std::string::npos);
}

TEST_F(ExperimentMetricsTest, SpectrometerJobsByKindAndHowTheyEnded) {
  EXPECT_DOUBLE_EQ(value(text(), "pychron_spectrometer_jobs_total{kind=\"peak_center\",state=\"failed\"}"), 0.0);
  bus.publish(pychron::jobs::JobStarted{1, "peak_center", {}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_spectrometer_job_active"), 1.0);
  bus.publish(pychron::jobs::JobProgress{1, "peak_center", pychron::jobs::ProgressUpdate{5, 20, "", std::nullopt}});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_spectrometer_job_progress_ratio"), 0.25);

  pychron::jobs::Job job;
  job.id = 1;
  job.kind = "peak_center";
  job.state = pychron::jobs::JobState::Succeeded;
  job.before = pychron::jobs::SpectrometerState{};
  job.started = at(100);
  job.finished = at(140);
  bus.publish(pychron::jobs::JobFinished{job});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_spectrometer_jobs_total{kind=\"peak_center\",state=\"succeeded\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_spectrometer_job_active"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_spectrometer_job_progress_ratio"), 0.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_spectrometer_job_duration_seconds_sum{kind=\"peak_center\"}"), 40.0);
  EXPECT_DOUBLE_EQ(value(t, "pychron_spectrometer_job_duration_seconds_count{kind=\"peak_center\"}"), 1.0);
}

// Cancelled while it waited: it ended, and took no time doing anything.
TEST_F(ExperimentMetricsTest, AJobThatNeverRanIsCountedAndNotTimed) {
  pychron::jobs::Job job;
  job.kind = "sweep";
  job.state = pychron::jobs::JobState::Cancelled;
  job.finished = at(140);
  bus.publish(pychron::jobs::JobFinished{job});
  const std::string t = text();
  EXPECT_DOUBLE_EQ(value(t, "pychron_spectrometer_jobs_total{kind=\"sweep\",state=\"cancelled\"}"), 1.0);
  EXPECT_FALSE(has(t, "pychron_spectrometer_job_duration_seconds_count{kind=\"sweep\"}"));
}

TEST_F(ExperimentMetricsTest, EveryFamilyIsNamedBeforeItsFirstEvent) {
  const std::vector<std::string> names = registry.names();
  for (const char* expected :
       {"pychron_executor_state", "pychron_queue_active", "pychron_queue_runs", "pychron_queues_ended_total",
        "pychron_runs_started_total", "pychron_runs_finished_total", "pychron_run_save_errors_total",
        "pychron_run_state_duration_seconds", "pychron_measurement_blocks_total", "pychron_conditional_trips_total",
        "pychron_executor_waits_total", "pychron_last_run_finished_age_seconds",
        "pychron_notifications_total", "pychron_spectrometer_jobs_total", "pychron_spectrometer_job_active",
        "pychron_spectrometer_job_progress_ratio", "pychron_spectrometer_job_duration_seconds"}) {
    EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end()) << expected;
  }
}

TEST_F(ExperimentMetricsTest, StopsListeningWhenDestroyed) {
  metrics.reset();
  bus.publish(executor::RunStarted{0, "r", "i", at(0)});
  EXPECT_DOUBLE_EQ(value(text(), "pychron_runs_started_total"), 0.0);
}
