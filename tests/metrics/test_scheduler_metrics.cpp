#include "pychron/metrics/scheduler_metrics.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "metrics_text.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/metrics/registry.hpp"

using namespace pychron;
using namespace pychron::metrics;
using namespace std::chrono_literals;
using metrics_text::value;

namespace {

// Declared in the order the lifetime rule asks for: what the object under
// test points at comes first.
struct SchedulerMetricsTest : ::testing::Test {
  ManualClock clock;
  Scheduler scheduler{clock, nullptr, Scheduler::Options{0}};
  Registry registry;
  double now = 1000.0;

  void tick(Duration by) {
    clock.advance(by);
    scheduler.run_pending();
  }
};

const char* const kHeartbeat = "pychron_scheduler_heartbeat_timestamp_seconds";

}  // namespace

TEST_F(SchedulerMetricsTest, RendersEachJobsCounters) {
  int n = 0;
  ASSERT_TRUE(scheduler
                  .scan("gauges", 1s,
                        [&n]() -> Result<Sample> {
                          if (++n == 2) return fail(ErrorKind::Timeout, "no reply");
                          return Sample{"gauges", {}, 1.0};
                        })
                  .has_value());
  SchedulerMetrics metrics(registry, scheduler, [this] { return now; });
  for (int i = 0; i < 3; ++i) tick(1s);
  const std::string text = registry.render();
  EXPECT_DOUBLE_EQ(value(text, "pychron_scheduler_job_runs_total{job=\"gauges\"}"), 3.0);
  EXPECT_DOUBLE_EQ(value(text, "pychron_scheduler_job_failures_total{job=\"gauges\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(text, "pychron_scheduler_job_skipped_overlaps_total{job=\"gauges\"}"), 0.0);
}

TEST_F(SchedulerMetricsTest, TwoJobsWithOneNameAreAddedTogether) {
  ASSERT_TRUE(scheduler.every("twin", 1s, [] {}).has_value());
  ASSERT_TRUE(scheduler.every("twin", 1s, [] {}).has_value());
  SchedulerMetrics metrics(registry, scheduler, [this] { return now; });
  tick(1s);
  EXPECT_DOUBLE_EQ(value(registry.render(), "pychron_scheduler_job_runs_total{job=\"twin\"}"), 2.0);
  // A second scrape reads the same totals: nothing is counted twice.
  EXPECT_DOUBLE_EQ(value(registry.render(), "pychron_scheduler_job_runs_total{job=\"twin\"}"), 2.0);
}

TEST_F(SchedulerMetricsTest, AJobThatIsReplacedKeepsCounting) {
  const auto first = scheduler.every("scan", 1s, [] {});
  ASSERT_TRUE(first.has_value());
  SchedulerMetrics metrics(registry, scheduler, [this] { return now; });
  tick(1s);
  tick(1s);
  EXPECT_DOUBLE_EQ(value(registry.render(), "pychron_scheduler_job_runs_total{job=\"scan\"}"), 2.0);
  scheduler.cancel(*first);
  ASSERT_TRUE(scheduler.every("scan", 1s, [] {}).has_value());
  tick(1s);
  EXPECT_DOUBLE_EQ(value(registry.render(), "pychron_scheduler_job_runs_total{job=\"scan\"}"), 3.0);
}

TEST_F(SchedulerMetricsTest, HeartbeatFollowsTheScheduler) {
  SchedulerMetrics metrics(registry, scheduler, [this] { return now; });
  EXPECT_DOUBLE_EQ(value(registry.render(), kHeartbeat), 1000.0);
  now = 1005.0;
  tick(4s);
  EXPECT_DOUBLE_EQ(value(registry.render(), kHeartbeat), 1000.0) << "the heartbeat is every 5 s";
  tick(1s);
  EXPECT_DOUBLE_EQ(value(registry.render(), kHeartbeat), 1005.0);
}

TEST_F(SchedulerMetricsTest, AStoppedSchedulerLeavesTheHeartbeatBehind) {
  SchedulerMetrics metrics(registry, scheduler, [this] { return now; });
  now = 1300.0;  // real time passes, the scheduler dispatches nothing
  EXPECT_DOUBLE_EQ(value(registry.render(), kHeartbeat), 1000.0);
}

TEST_F(SchedulerMetricsTest, TheHeartbeatJobIsCountedOnce) {
  SchedulerMetrics metrics(registry, scheduler, [this] { return now; });
  tick(5s);
  EXPECT_DOUBLE_EQ(value(registry.render(), "pychron_scheduler_job_runs_total{job=\"metrics.heartbeat\"}"), 1.0);
}

TEST_F(SchedulerMetricsTest, DestructionCancelsTheJobAndTheCollector) {
  const std::size_t before = scheduler.job_count();
  {
    SchedulerMetrics metrics(registry, scheduler, [this] { return now; });
    EXPECT_EQ(scheduler.job_count(), before + 1);
  }
  EXPECT_EQ(scheduler.job_count(), before);
  ASSERT_TRUE(scheduler.every("later", 1s, [] {}).has_value());
  tick(1s);
  EXPECT_FALSE(metrics_text::has(registry.render(), "pychron_scheduler_job_runs_total{job=\"later\"}"));
}
