#pragma once

// JobRunner: one tuning job at a time per spectrometer, queue depth 1. A
// submit while a job is queued or running fails with Error{Interlock,
// "spectrometer busy"}. The job body runs on a Scheduler worker; run()
// executes one synchronously on the caller's thread (CLI) under the same
// interlock.
//
// Per job: JobStarted (with the "before" SpectrometerState snapshot), a
// JobProgress per update, JobFinished (with "after") on the SignalBus.

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/systems/jobs/job.hpp"

namespace pychron::jobs {

class JobRunner {
 public:
  struct Options {
    std::size_t history = 16;  // finished jobs kept for job()
  };

  JobRunner(Spectrometer& spectrometer, Scheduler& scheduler, SignalBus& bus, const Clock& clock);
  JobRunner(Spectrometer& spectrometer, Scheduler& scheduler, SignalBus& bus, const Clock& clock,
            Options options);
  // Cancels the current job and waits for its body to return.
  ~JobRunner();
  JobRunner(const JobRunner&) = delete;
  JobRunner& operator=(const JobRunner&) = delete;

  // Config error for an empty body; Interlock when busy.
  Result<JobId> submit(JobSpec spec);
  // Runs on the calling thread; returns the finished job.
  Result<Job> run(JobSpec spec);

  // Queued: finishes Cancelled without running. Running: signals the token
  // (the body returns at its next check). Config error for an unknown or
  // finished job.
  Result<void> cancel(JobId id);

  bool busy() const;
  std::optional<JobId> current() const;
  // Current or recently finished job.
  std::optional<Job> job(JobId id) const;

  // Blocks until no job is queued or running.
  void wait_idle() const;

 private:
  struct Slot;

  Result<std::shared_ptr<Slot>> reserve(JobSpec spec);
  void execute(const std::shared_ptr<Slot>& slot);
  void finish(const std::shared_ptr<Slot>& slot);

  Spectrometer& spectrometer_;
  Scheduler& scheduler_;
  SignalBus& bus_;
  const Clock& clock_;
  Options options_;

  mutable std::mutex mutex_;
  mutable std::condition_variable idle_;
  std::shared_ptr<Slot> current_;
  std::deque<Job> history_;
  JobId next_id_ = 1;
};

}  // namespace pychron::jobs
