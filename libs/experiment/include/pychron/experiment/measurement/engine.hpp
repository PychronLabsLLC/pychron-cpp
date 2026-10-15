#pragma once

// MeasurementEngine (experiment spec 4.2): executes a resolved
// MeasurementPlan over spectrometer primitives and extraction-line valves.
//
// Fixed block sequence; a disabled block is skipped:
//
//   peak_center.before -> baseline.before -> position first hop
//   -> equilibrate || sniff -> time zero -> main (cycles x hops)
//   -> baseline.after -> peak_center.after
//
// One HopRunner executes every collection: protect the hop's `protect` set ->
// position (skipped, with its settle, when the magnet is already there) ->
// settle -> unprotect -> collect `counts` readings on the hop's detectors.
// main runs it `cycles` times over the hop list; sniff and the before/after
// baselines are single-hop runs of the same runner. There is no separate
// multicollect path: one hop x N cycles collects exactly what one hop with
// N x counts does.
//
// Equilibration (single-threaded, so sniff readings and valve timing never
// race): close the outlet, wait inlet_delay_s, open the inlet, wait time_s,
// close the inlet (if close_inlet). Sniff readings on the first hop's
// detectors are taken while the waits run, so valve transitions happen at
// reading granularity. Closing the inlet publishes OverlapReady and calls
// on_inlet_closed (the run layer starts post-equilibration there). An
// equilibration conditional tripping during sniff closes the inlet early.
//
// Time zero: on_inlet_close (inlet close, or end of time_s when close_inlet
// is false), on_first_count (first main reading), offset_s (inlet open +
// offset_s).
//
// Conditionals (conditionals spec section 6.1) are evaluated after every main
// signal reading in pychron's order (modification, truncation, action,
// termination, cancelation) with the main-wide reading count, and
// equilibration conditionals after every sniff reading; the first trip ends
// the reading's evaluation. What a trip does:
//   truncation     truncate main with the conditional's abbreviated_count_ratio
//   termination    end the measurement, data kept (outcome Terminated)
//   cancelation    end it, outcome Cancelled with cancel_queue
//   action         truncate / terminate / cancel / set_param / run_hook /
//                  notify; without `resume` the action also ends main
//   modification   reported for the executor; `truncate` / `terminate` act now
//   equilibration  close the inlet now
//
// Truncate (truncate(), a truncation trip, or a truncate action) ends the
// current collection at the next reading. Truncating main skips its remaining
// hops and cycles, and later collections scale their counts by the ratio in
// effect (the conditional's abbreviated_count_ratio; a user truncate 1.0;
// quick 0.25), rounded up, at least 1.
//
// Whiff (plan [whiff]): once the inlet opens, `counts` readings of kind whiff
// on the first hop's detectors (sniff starts after them), then the first
// matching check decides: run_remainder continues; pump closes the inlet,
// opens the outlet and ends the measurement (Terminated); abort ends it
// (Aborted). The hook's on_whiff_result receives the result.
//
// Safety, on every exit including cancel, abort and failure: acquisition is
// stopped, detectors the engine protected are unprotected, and an inlet the
// engine opened is closed.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/collect/collector.hpp"
#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/measurement/keep_trying.hpp"
#include "pychron/experiment/measurement/ports.hpp"
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/scripting/cancel_token.hpp"

namespace pychron::experiment::measurement {

enum class Block { PeakCenterBefore, BaselineBefore, PositionFirstHop, Equilibrate, Main, BaselineAfter, PeakCenterAfter };

std::string_view to_string(Block block) noexcept;

enum class MeasurementOutcome {
  Completed,
  Truncated,   // main ended early; later blocks ran with scaled counts
  Terminated,  // a termination ended the measurement; data kept
  Cancelled,   // CancelToken::cancel() or a cancelation trip
  Aborted,     // CancelToken::abort()
  Failed,      // hardware or configuration error; see MeasurementResult::error
};

std::string_view to_string(MeasurementOutcome outcome) noexcept;

// ---- events (SignalBus) ----------------------------------------------------

struct BlockStarted {
  std::string run_id;
  Block block = Block::Main;
};
struct BlockFinished {
  std::string run_id;
  Block block = Block::Main;
  bool ok = true;
};
// The readings stopped (spec 4.4). Seconds since the measurement started:
// from the last reading before the silence to the first after it, or, when
// none came, to when the collection gave up on it.
struct MeasurementGap {
  std::string run_id;
  Block block = Block::Main;
  double from = 0;
  double to = 0;
  bool resumed = true;
  TimePoint at{};  // when the gap was closed, on the engine's clock
};

// "main: no readings from 30.0 s to 51.0 s; they resumed".
std::string describe(const MeasurementGap& gap);

struct CountsProgress {
  std::string run_id;
  Block block = Block::Main;
  int i = 0;  // readings so far in the current collection (hop, baseline, sniff)
  int n = 0;  // its target
};
// The inlet closed: the executor may start the next run's extraction.
struct OverlapReady {
  std::string run_id;
};
struct ConditionalTripped {
  std::string run_id;
  Trip trip;
};

// ---- inputs and result -----------------------------------------------------

// Everything the engine touches. Only `spectrometer` is required; `valves` is
// required when the plan names an inlet or outlet, `peak_center` when it
// enables a peak-center block, `hook` when it names a hook.
struct EngineContext {
  ISpectrometerPort& spectrometer;
  const Clock& clock;
  IValvePort* valves = nullptr;
  IPeakCenterPort* peak_center = nullptr;
  IMeasurementHook* hook = nullptr;
  SignalBus* bus = nullptr;
  // Metrics the Collector cannot answer (gauges, devices, detector state).
  const MetricContext* metrics = nullptr;
};

struct EngineOptions {
  // Blocking wait. Default: CancelToken::wait_until on the context clock.
  // Tests inject one that advances a ManualClock.
  std::function<void(pychron::Duration)> sleep;
  // Reading timeout = integration x timeout_factor + timeout_slack. A
  // reading that does not come within it starts a gap (spec 4.4): the
  // collection waits on the clock, doing nothing else, for up to max_gap from
  // the last reading, or to its planned end; then the acquisition is started
  // again once, and the collection ends if that gives nothing.
  double timeout_factor = 3.0;
  pychron::Duration timeout_slack = std::chrono::seconds(5);
  pychron::Duration max_gap = std::chrono::minutes(2);
  // After a gap in main: fewer signal counts than this share of the planned
  // ones (and never fewer than the fit needs) fail the measurement.
  double min_counts_fraction = 0.2;
  // The inlet close and a detector's unprotect keep trying this long (spec
  // 4.4, valve row), with an alarm at critical_alarm_after.
  pychron::Duration critical_window = kCriticalWindow;
  pychron::Duration critical_alarm_after = kCriticalAlarmAfter;
  // Called on the engine thread right after the inlet closes.
  std::function<void()> on_inlet_closed;
  // Called on the engine thread as a block begins, before its BlockStarted
  // is published: for the engine's owner, whose own state must not depend on
  // there being a bus.
  std::function<void(Block)> on_block_started;
};

struct MeasurementInputs {
  plan::MeasurementPlan plan;
  // Merged system -> queue -> plan -> run set (see plan_conditionals()).
  ConditionalSet conditionals;
  Variables variables;
  std::string run_id;
  std::string analysis_type;  // filters conditionals by analysis_types
  std::map<std::string, double> icfactors;              // by detector, for live corrected values
  std::optional<reduction::ArArConstants> arar;         // enables age, kca, ... in checks
};

struct MeasurementResult {
  MeasurementOutcome outcome = MeasurementOutcome::Completed;
  std::optional<Error> error;  // Failed only
  collect::RunData data;
  std::vector<PeakCenterReport> peak_centers;
  std::vector<Trip> modifications;        // for the executor (skip, repeat, run blank, ...)
  bool cancel_queue = false;              // a cancelation (conditional) asks to stop the queue
  std::optional<WhiffCheck::Action> whiff;  // when the plan whiffs
  std::vector<Conditional> installed;     // conditionals that applied to this run
  std::vector<std::string> notes;         // notify actions, hook log lines
  std::vector<ConditionalError> conditional_errors;  // checks that could not be evaluated
  double count_scale = 1.0;               // after a truncation of main
  std::vector<Block> blocks;              // blocks that ran, in order
  std::vector<MeasurementGap> gaps;       // silences in the readings, in order
  bool truncated_by_link = false;         // main ended by a silence that did not end (Truncated)
};

// The plan's own [conditionals].truncations as a ConditionalSet level (names
// "plan.truncation[i]"). include references are resolved by the caller.
Result<ConditionalSet> plan_conditionals(const plan::MeasurementPlan& plan);

class MeasurementEngine {
 public:
  MeasurementEngine(EngineContext context, MeasurementInputs inputs, EngineOptions options = {});
  ~MeasurementEngine();
  MeasurementEngine(const MeasurementEngine&) = delete;
  MeasurementEngine& operator=(const MeasurementEngine&) = delete;

  // Checks the context covers the plan (valves, peak center, hook). Run calls
  // it first; callers may call it early.
  Result<void> validate() const;

  // Runs the block sequence once on the calling thread.
  MeasurementResult run(scripting::CancelToken& token);

  // Thread-safe controls for the running measurement.
  void truncate(bool quick = false);
  void set_target(int counts) { collector_.set_target(counts); }

  const collect::Collector& collector() const noexcept { return collector_; }

 private:
  class Api;
  enum class Stop { None, Terminate, Cancel, Abort };

  // Block steps. A non-ok result is a Failed measurement; a Stop is read from
  // stop_ after each step.
  Result<void> run_blocks();
  Result<void> peak_center(Block block);
  Result<void> baseline(Block block);
  Result<void> equilibrate();
  Result<void> whiff();
  Result<void> main();
  Result<void> call_hook(std::string_view entry, const scripting::ValueMap& args = {});

  Result<void> move_to(const std::optional<plan::HopTarget>& target, double settle_s,
                       const std::vector<std::string>& protect);
  Result<collect::CollectStatus> collect(Block block, collect::CollectionSpec spec, double integration_s,
                                         collect::ReadingHook hook);
  bool main_reading(int count, double t);

  // A silence in the readings (spec 4.4).
  enum class GapEnd { Resumed, Cap, NotResumed };
  struct GapWait {
    GapEnd end = GapEnd::Cap;
    std::optional<spectrometer::Reading> reading;  // Resumed
  };
  // Waits for readings to resume, on the clock, until max_gap from the
  // silence's start or `cap` (Cap: the gap stays open for the caller), and
  // past max_gap starts the acquisition again once (NotResumed when that
  // gives nothing). A definite error is returned.
  Result<GapWait> wait_out_gap(Block block, pychron::Duration integration, pychron::Duration timeout, TimePoint cap);
  void close_gap(Block block, TimePoint to, bool resumed);
  // start_acquisition, started again once when its outcome is unknown.
  Result<void> start_acquisition(pychron::Duration integration);
  // A call that must get through (spec 4.4, valve row).
  Result<void> critical(const std::string& what, const std::function<Result<void>()>& op);
  Result<void> close_inlet();
  Result<void> check_main_counts();
  bool sniff_reading();
  void handle_trip(const Trip& trip);
  void request_truncate(double ratio);
  bool wait(double seconds);  // false when cancelled
  bool stopping() const;
  void publish_log(LogLevel level, std::string message);
  int scaled(int counts) const;
  std::vector<collect::Channel> baseline_channels() const;
  void rebuild_conditionals();
  void cleanup(bool abnormal);

  EngineContext ctx_;
  MeasurementInputs in_;
  EngineOptions options_;
  collect::Collector collector_;

  scripting::CancelToken* token_ = nullptr;
  std::unique_ptr<ConditionalEngine> conditionals_;
  MeasurementResult result_;
  std::optional<plan::HopTarget> position_;  // where the magnet is, when known
  std::set<std::string> protected_;
  std::vector<collect::Channel> last_channels_;
  bool acquiring_ = false;
  bool inlet_open_ = false;
  bool close_inlet_now_ = false;
  int main_count_ = 0;
  Stop stop_ = Stop::None;
  bool main_truncated_ = false;
  bool break_main_ = false;  // a non-resuming action ended main
  TimePoint started_{};                 // the measurement's start, time 0 of the collector
  TimePoint last_reading_{};            // of the current collection (its start before any)
  std::optional<TimePoint> gap_from_;   // a silence not yet closed
  std::string silence_error_;           // what the port said while silent
  bool main_gap_ = false;               // main had a gap
  bool main_cut_ = false;               // a silence in main did not end

  std::mutex truncate_mutex_;
  std::optional<double> truncate_request_;  // count ratio for later collections
  std::atomic<bool> collecting_{false};
};

}  // namespace pychron::experiment::measurement
