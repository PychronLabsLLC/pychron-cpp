#pragma once

// The scheduler as the box sees it: each job's counters, read at every
// scrape, and a heartbeat that says the scheduler is still dispatching.
//
// The endpoint answers from a thread of its own, so a scrape succeeds even
// when the scheduler has stopped. The heartbeat is what tells the two apart:
// a job notes the time every five seconds (of the line's clock), and each
// scrape reports how long ago that was. The age is measured here, on one
// clock, so an alert on it does not depend on the box's clock.

#include <optional>

#include "pychron/core/scheduler.hpp"
#include "pychron/metrics/registry.hpp"

namespace pychron::metrics {

class SchedulerMetrics {
 public:
  // `registry` and `scheduler` must outlive this object.
  SchedulerMetrics(Registry& registry, Scheduler& scheduler, RealClock now = steady_real_clock());
  ~SchedulerMetrics();  // cancels the heartbeat job, removes the collector
  SchedulerMetrics(const SchedulerMetrics&) = delete;
  SchedulerMetrics& operator=(const SchedulerMetrics&) = delete;

 private:
  Scheduler& scheduler_;
  std::optional<JobId> heartbeat_;
  CollectorHandle collector_;
};

}  // namespace pychron::metrics
