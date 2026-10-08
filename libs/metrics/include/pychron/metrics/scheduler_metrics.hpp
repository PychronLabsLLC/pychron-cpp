#pragma once

// The scheduler as the box sees it: each job's counters, read at every
// scrape, and a heartbeat that says the scheduler is still dispatching.
//
// The endpoint answers from a thread of its own, so a scrape succeeds even
// when the scheduler has stopped. The heartbeat is what tells the two apart:
// a job sets a gauge to the real time every five seconds (of the line's
// clock), and an alert compares that gauge with the time of the scrape.

#include <optional>

#include "pychron/core/scheduler.hpp"
#include "pychron/metrics/registry.hpp"

namespace pychron::metrics {

class SchedulerMetrics {
 public:
  // `registry` and `scheduler` must outlive this object.
  SchedulerMetrics(Registry& registry, Scheduler& scheduler, UnixClock now = system_unix_clock());
  ~SchedulerMetrics();  // cancels the heartbeat job, removes the collector
  SchedulerMetrics(const SchedulerMetrics&) = delete;
  SchedulerMetrics& operator=(const SchedulerMetrics&) = delete;

 private:
  Scheduler& scheduler_;
  std::optional<JobId> heartbeat_;
  CollectorHandle collector_;
};

}  // namespace pychron::metrics
