#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/logger.hpp"

namespace pychron {

class SignalBus;

using JobId = std::uint64_t;

struct JobStats {
  std::uint64_t runs = 0;              // completed executions
  std::uint64_t failures = 0;          // scans returning an error, or tasks that threw
  std::uint64_t skipped_overlaps = 0;  // due while the previous run was still going
};

// Owns all periodic work in the process. Drivers and managers own no threads.
//
// Job kinds:
//   every()    - periodic task, fixed rate; missed ticks are dropped, not queued
//   scan()     - periodic sampler; each Ok sample is published as `Sample`
//   after()    - one-shot delayed task
//   watchdog() - calls `on_missed` whenever `timeout` elapses without heartbeat()
//
// Per-job non-overlap: a job never runs concurrently with itself. If a job is
// due while its previous run is still executing, that tick is skipped and
// counted in JobStats::skipped_overlaps.
//
// Time comes from the injected Clock. Due jobs are dispatched either by the
// background dispatcher (start()) or explicitly via run_pending(), which is
// what deterministic tests with a ManualClock use.
//
// Every wait and every notify of the scheduler goes through that clock, and
// its threads are participants in it ("scheduler.dispatch", "scheduler.worker"):
// on a VirtualClock time stands still while a job runs and jumps to the next
// due time when the dispatcher, the workers and every other participant are
// waiting. A thread that calls run_pending() with no worker pool runs the jobs
// itself and is a participant only if its caller made it one. stop() and the
// destructor wait in the clock for their threads to say they have finished
// and only then join them: a job still running may use up clock time
// meanwhile, and time does not move on account of the wait itself.
class Scheduler {
 public:
  struct Options {
    // Worker pool size. 0 runs jobs inline on the thread calling run_pending().
    std::size_t threads = 4;
  };

  using Task = std::function<void()>;
  using Sampler = std::function<Result<Sample>()>;

  explicit Scheduler(const Clock& clock, SignalBus* bus = nullptr);
  // With `log_hub`, the scheduler's own records (failed scans, throwing jobs)
  // go through the hub's "scheduler" logger: file, level rules, stderr echo
  // and the hub's bus. They are also published on `bus` if that is a
  // different bus. Without a hub they go straight to `bus`.
  Scheduler(const Clock& clock, SignalBus* bus, Options options,
            std::shared_ptr<LogHub> log_hub = nullptr);
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // First run is one `interval` after registration. interval must be > 0.
  Result<JobId> every(std::string name, Duration interval, Task task);
  // `device` names the Sample when the sampler leaves it empty; a zero
  // Sample::ts is filled with the clock's time. Errors are logged, not published as samples.
  Result<JobId> scan(std::string device, Duration interval, Sampler sampler);
  Result<JobId> after(std::string name, Duration delay, Task task);
  Result<JobId> watchdog(std::string name, Duration timeout, Task on_missed);

  // Re-arms a watchdog. Cancelled if the job no longer exists; Config if not a watchdog.
  Result<void> heartbeat(JobId id);

  // Removes a job. An execution already in progress finishes normally.
  bool cancel(JobId id);

  // Dispatches every job due at clock.now(); returns how many were dispatched.
  std::size_t run_pending();

  // Background dispatcher thread that calls run_pending() as jobs fall due.
  // stop() waits for that thread, so a job running on it (no worker pool)
  // cannot call it: it throws std::system_error with
  // resource_deadlock_would_occur, which makes the job a failed one, and the
  // scheduler goes on dispatching.
  void start();
  void stop();
  bool started() const;

  // Blocks until no dispatched job is queued or executing.
  void wait_idle();

  std::optional<JobStats> stats(JobId id) const;
  std::size_t job_count() const;

 private:
  enum class Kind { Periodic, OneShot, Watchdog };
  struct Job;

  Result<JobId> add(std::string name, Kind kind, Duration period, std::function<bool()> body);
  std::vector<std::shared_ptr<Job>> collect_due_locked(TimePoint now);
  void execute(const std::shared_ptr<Job>& job);
  void publish_log(LogLevel level, std::string message) const;
  // `started` is the starter's Clock::Hold, dropped once the thread has
  // entered the clock.
  void worker_loop(std::shared_ptr<Clock::Hold> started);
  void dispatcher_loop(std::shared_ptr<Clock::Hold> started);

  const Clock& clock_;
  SignalBus* bus_;
  Options options_;
  std::shared_ptr<LogHub> log_hub_;
  std::optional<Logger> logger_;  // "scheduler" on log_hub_

  mutable std::mutex mutex_;
  std::condition_variable wake_;       // dispatcher: jobs changed or stop requested
  std::condition_variable work_ready_;  // workers: queue non-empty or shutdown
  std::condition_variable idle_;       // wait_idle(): in_flight_ reached zero
  std::condition_variable exited_;     // stop(), destructor: a thread has finished
  std::map<JobId, std::shared_ptr<Job>> jobs_;
  std::deque<std::shared_ptr<Job>> queue_;
  std::size_t in_flight_ = 0;
  JobId next_id_ = 1;
  bool dispatching_ = false;
  bool dispatcher_done_ = true;   // the dispatcher thread has left its loop
  std::size_t live_workers_ = 0;  // worker threads that have not left theirs
  bool shutting_down_ = false;
  std::vector<std::thread> workers_;
  std::thread dispatcher_;
};

}  // namespace pychron
