#pragma once

// Executor (experiment spec 3.2, 3.3): runs an ExperimentQueue.
//
//   Idle -> Preparing -> Running -> [StoppingAtBoundary | Cancelling | Aborting] -> Finalizing -> Idle
//
// Per row: skipped rows are passed over; a pause row waits its extraction
// duration; otherwise the delay policy, the pre-run checks (IPreRunCheck
// plugins, then the queue's pre_run conditionals), then the run.
//
// Delay before a run: the previous run's delay_after if set, else
// delays.after_blank after a blank, else delays.between_analyses;
// delays.before_analyses before the first run. No delay while the previous
// run is still measuring (overlap).
//
// After a run:
//   - modification trips' queue actions and post_run conditionals are
//     applied to the queue (after the last started row);
//   - a cancelation trip, a pre_run trip, a post_run cancel, a cancelled or
//     aborted run end the queue; a failed run ends it unless the failure was
//     only a save error (the record is in the spool) or continue_on_failure;
//   - end_after ends it after this run.
//
// Overlap: an unknown run with overlap.duration > 0 that is not the last
// runnable row lets the next run start overlap.duration after its inlet
// closes. At most two runs are in flight. The extraction device and the
// spectrometer are resources: the next run's extraction waits for the
// device, its measurement for the spectrometer and for overlap.min_delay of
// pump time since the previous run's post-measurement started.
//
// executor_state.json is rewritten on every transition and run start; a row
// counts as consumed when its run starts, so resume_row() never re-runs a
// partially measured run.

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/model/experiment_queue.hpp"
#include "pychron/experiment/run/run.hpp"
#include "pychron/scripting/cancel_token.hpp"

namespace pychron::experiment::executor {

enum class ExecutorState { Idle, Preparing, Running, StoppingAtBoundary, Cancelling, Aborting, Finalizing };
std::string_view to_string(ExecutorState s) noexcept;

// A pluggable check before every run (managers healthy, disk space, ...).
class IPreRunCheck {
 public:
  virtual ~IPreRunCheck() = default;
  virtual std::string name() const = 0;
  virtual Result<void> check(const RunSpec& run) = 0;
};

struct ExecutorStateChanged {
  ExecutorState from = ExecutorState::Idle, to = ExecutorState::Idle;
  std::string reason;
};
struct RunStarted {
  std::size_t row = 0;
  std::string run_id, identifier;
};
struct RunSummary {
  std::size_t row = 0;
  std::string identifier, run_id;
  int aliquot = 0;
  std::string step;
  run::RunState state = run::RunState::Pending;
  bool truncated = false, save_error = false;
  std::optional<std::string> error;
  std::vector<std::string> queue_changes;  // descriptions of applied queue actions
};
struct RunFinished {
  RunSummary summary;
};
// A run's queue actions or post-run conditionals changed the queue.
// Published before that run's RunFinished, with a copy of the whole queue.
struct QueueEdited {
  QueueSpec queue;
  std::vector<std::string> changes;  // as RunSummary::queue_changes
};
// The executor is waiting: a delay, a scheduled start, a resource, pump time.
struct ExecutorWaiting {
  std::string reason;
  Duration duration{};
};

enum class QueueEnd { Completed, Stopped, Cancelled, Aborted, Failed };
std::string_view to_string(QueueEnd e) noexcept;

struct QueueResult {
  QueueEnd end = QueueEnd::Completed;
  std::string reason;
  std::vector<RunSummary> runs;
};

struct ExecutorContext {
  run::RunServices services;                         // copied into every run
  const MetricContext* pre_run_metrics = nullptr;    // instrument metrics for pre_run conditionals
  std::vector<IPreRunCheck*> checks;
  BlankFactory blank = default_blank_factory(IdentifierRules::defaults());
};

struct ExecutorOptions {
  std::filesystem::path state_file;  // executor_state.json; empty: not written
  // Timed waits (delays, overlap, pump time, scheduled start). Default: wait
  // on the services clock, cut short by cancel/abort. Tests advance a ManualClock.
  std::function<void(Duration)> sleep;
  std::optional<TimePoint> start_at;  // wait before the first run
  std::optional<TimePoint> stop_at;   // stop at the first run boundary after it
  bool allow_overlap = true;
  bool continue_on_failure = false;
};

class Executor {
 public:
  Executor(ExecutorContext context, ExecutorOptions options = {});
  ~Executor();
  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  // Runs `queue` from `from_row` on the calling thread until it ends.
  QueueResult execute(ExperimentQueue& queue, std::size_t from_row = 0);

  // Thread-safe controls.
  void stop();                       // finish the current run(s), start no more
  void cancel();                     // cancel the active run(s) and end the queue
  void abort();                      // abort the active run(s) and end the queue
  void truncate(bool quick = false); // the run currently measuring
  ExecutorState state() const;

  // The row to resume from after a restart (one past the last started run).
  static Result<std::size_t> resume_row(const std::filesystem::path& state_file);

 private:
  struct Slot;
  class Resource;

  void set_state(ExecutorState to, std::string reason = {});
  bool wait(Duration d, const std::string& reason);  // false when cancelled/aborted
  bool ending() const;
  bool overlaps(const ExperimentQueue& queue, std::size_t row) const;
  std::unique_ptr<Slot> launch(ExperimentQueue& queue, std::size_t row, int index);
  void finish(ExperimentQueue& queue, Slot& slot, QueueResult& out);
  void write_state(const ExperimentQueue& queue, std::size_t next_row, const QueueResult& out);

  ExecutorContext ctx_;
  ExecutorOptions options_;
  const Clock& clock_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  ExecutorState state_ = ExecutorState::Idle;
  bool stop_ = false;
  std::optional<QueueEnd> end_;  // why the queue is ending early
  std::string end_reason_;
  scripting::CancelToken queue_token_;  // cuts executor waits short
  std::vector<Slot*> active_;

  std::unique_ptr<Resource> extraction_, spectrometer_;
  std::optional<TimePoint> pump_started_;
  std::optional<RunChecks> run_checks_;
  std::size_t last_started_row_ = 0;
  std::optional<RunSpec> previous_spec_;  // last run that ran (for the delay policy)
};

}  // namespace pychron::experiment::executor
