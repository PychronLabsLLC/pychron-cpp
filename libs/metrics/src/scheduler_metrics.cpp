#include "pychron/metrics/scheduler_metrics.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace pychron::metrics {

namespace {

constexpr const char* kRuns = "pychron_scheduler_job_runs_total";
constexpr const char* kFailures = "pychron_scheduler_job_failures_total";
constexpr const char* kSkipped = "pychron_scheduler_job_skipped_overlaps_total";
constexpr const char* kRunsHelp = "Completed executions of a scheduler job.";
constexpr const char* kFailuresHelp = "Executions of a scheduler job that failed: a scan's error, a task that threw.";
constexpr const char* kHeartbeatAge = "pychron_scheduler_heartbeat_age_seconds";
constexpr const char* kHeartbeatAgeHelp =
    "Real seconds since the scheduler last ran its heartbeat job. It grows when the scheduler has stopped.";
constexpr const char* kSkippedHelp = "Times a scheduler job was due while its previous run was still going.";

}  // namespace

SchedulerMetrics::SchedulerMetrics(Registry& registry, Scheduler& scheduler, RealClock now) : scheduler_(scheduler) {
  registry.declare(MetricType::Counter, kRuns, kRunsHelp);
  registry.declare(MetricType::Counter, kFailures, kFailuresHelp);
  registry.declare(MetricType::Counter, kSkipped, kSkippedHelp);

  registry.declare(MetricType::Gauge, kHeartbeatAge, kHeartbeatAgeHelp);
  // Shared with the job: cancel() does not wait for a run in progress, so
  // the job may still be writing when this object has gone.
  auto last_beat = std::make_shared<std::atomic<double>>(now());
  if (auto id = scheduler.every("metrics.heartbeat", std::chrono::seconds(5),
                                [last_beat, now] { last_beat->store(now(), std::memory_order_relaxed); })) {
    heartbeat_ = *id;
  }

  collector_ = registry.add_collector([&scheduler, last_beat, now](Registry& r) {
    r.gauge(kHeartbeatAge, kHeartbeatAgeHelp).set(now() - last_beat->load(std::memory_order_relaxed));

    // Two jobs may carry one name; a series must be given one total.
    std::map<std::string, JobStats> by_name;
    for (NamedJobStats& job : scheduler.job_stats()) {
      JobStats& sum = by_name[std::move(job.name)];
      sum.runs += job.stats.runs;
      sum.failures += job.stats.failures;
      sum.skipped_overlaps += job.stats.skipped_overlaps;
    }
    for (const auto& [name, stats] : by_name) {
      const Labels labels{{"job", name}};
      r.counter(kRuns, kRunsHelp, labels).set_total(static_cast<double>(stats.runs));
      r.counter(kFailures, kFailuresHelp, labels).set_total(static_cast<double>(stats.failures));
      r.counter(kSkipped, kSkippedHelp, labels).set_total(static_cast<double>(stats.skipped_overlaps));
    }
  });
}

SchedulerMetrics::~SchedulerMetrics() {
  collector_.reset();
  if (heartbeat_) scheduler_.cancel(*heartbeat_);
}

}  // namespace pychron::metrics
