#include "pychron/metrics/scheduler_metrics.hpp"

#include <chrono>
#include <map>
#include <string>
#include <utility>

namespace pychron::metrics {

namespace {

constexpr const char* kRuns = "pychron_scheduler_job_runs_total";
constexpr const char* kFailures = "pychron_scheduler_job_failures_total";
constexpr const char* kSkipped = "pychron_scheduler_job_skipped_overlaps_total";
constexpr const char* kRunsHelp = "Completed executions of a scheduler job.";
constexpr const char* kFailuresHelp = "Executions of a scheduler job that failed: a scan's error, a task that threw.";
constexpr const char* kSkippedHelp = "Times a scheduler job was due while its previous run was still going.";

}  // namespace

SchedulerMetrics::SchedulerMetrics(Registry& registry, Scheduler& scheduler, UnixClock now) : scheduler_(scheduler) {
  registry.declare(MetricType::Counter, kRuns, kRunsHelp);
  registry.declare(MetricType::Counter, kFailures, kFailuresHelp);
  registry.declare(MetricType::Counter, kSkipped, kSkippedHelp);

  Gauge& beat = registry.gauge("pychron_scheduler_heartbeat_timestamp_seconds",
                               "When the scheduler last ran the heartbeat job, in real time.");
  beat.set(now());
  if (auto id = scheduler.every("metrics.heartbeat", std::chrono::seconds(5), [&beat, now] { beat.set(now()); })) {
    heartbeat_ = *id;
  }

  collector_ = registry.add_collector([&scheduler](Registry& r) {
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
