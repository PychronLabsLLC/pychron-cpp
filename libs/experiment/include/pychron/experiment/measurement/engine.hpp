#pragma once

// MeasurementEngine (experiment spec 4.2): executes a resolved
// MeasurementPlan over spectrometer primitives and extraction-line valves.
//
// The engine runs the plan's first gas (measurement plan v2 spec, 5.1; later
// gases and every `when` come with the phases). Its segments run by `when`,
// in file order within one `when`, in the v1 block order:
//
//   before_inlet (peak_center steps, baseline segments)
//   -> position the main segment's first position
//   -> equilibrate || sniff segment (whiff segments once the inlet opens)
//   -> time zero -> main (the first after_inlet signal segment, cycles x
//   positions) -> the other after_inlet signal segments -> after_main
//   -> after_run
//
// Each segment or step is published as a block: a plan desugared from v1
// publishes exactly the v1 sequence (peak_center.before, baseline.before,
// position, equilibrate, main, baseline.after, peak_center.after); any other
// segment publishes Segment and any other step Step.
//
// One position executor (run_position) runs every collection (spec 5.2):
// select the position's field table (its own, else its segment's, else the
// configured current one) -> apply its optics -> protect the entries that
// carry `protect` -> set the deflections entries carry -> position (label,
// mass, DAC or HV) -> settle -> unprotect -> collect `counts` readings on the
// active entries, restarting acquisition with the position's integration
// (traced when it differs from the previous collection's) -> restore each
// deflection this position changed that the next position of the segment
// (cyclically) does not set. Table, optics, protect, move and settle are
// skipped when target, optics, table and deflections are what the previous
// position left; at a segment's end every deflection still changed is
// restored. A sniff or whiff reads where the magnet is, with none of this. A
// segment runs the executor `cycles` times over its positions. There is no
// separate multicollect path: one position x N cycles collects exactly what
// one position with N x counts does. What the engine did is in
// MeasurementResult::trace.
//
// Equilibration (single-threaded, so sniff readings and valve timing never
// race): close the outlet, wait inlet_delay_s, open the inlet, wait time_s,
// close the inlet (if close_inlet). The sniff segment's readings are taken
// while the waits run, so valve transitions happen at
// reading granularity. Closing the inlet publishes OverlapReady and calls
// on_inlet_closed (the run layer starts post-equilibration there). An
// equilibration conditional tripping during sniff closes the inlet early.
//
// Time zero (the main segment's, else the gas's): on_inlet_close (inlet
// close, or end of time_s when close_inlet is false), on_first_count (first
// main reading), offset_s (inlet open + offset_s).
//
// Conditionals (conditionals spec section 6.1) are evaluated after every
// signal reading of a signal segment in pychron's order (modification, truncation, action,
// termination, cancelation) with the measurement-wide signal reading count, and
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
// positions and cycles, and later collections scale their counts by the ratio in
// effect (the conditional's abbreviated_count_ratio; a user truncate 1.0;
// quick 0.25), rounded up, at least 1.
//
// Whiff (a whiff segment, v1 [whiff]): once the inlet opens, its position's
// readings of kind whiff (sniff starts after them), then the first
// matching check decides: run_remainder continues; pump closes the inlet,
// opens the outlet and ends the measurement (Terminated); abort ends it
// (Aborted). The hook's on_whiff_result receives the result.
//
// Safety, on every exit including cancel, abort and failure: acquisition is
// stopped, detectors the engine protected are unprotected, deflections it
// changed are restored, and an inlet the engine opened is closed.

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

// The v1 blocks, then Segment and Step for any other segment or step, and
// the staged-release blocks of a gas (Prepare, PumpOut; not run yet).
enum class Block {
  PeakCenterBefore,
  BaselineBefore,
  PositionFirstHop,
  Equilibrate,
  Main,
  BaselineAfter,
  PeakCenterAfter,
  Segment,
  Step,
  Prepare,
  PumpOut,
};

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

// `gas`: the gas's name (empty for a plan without [[gas]]); `segment`: the
// segment's index within the gas, -1 for Equilibrate and PositionFirstHop.
struct BlockStarted {
  std::string run_id;
  Block block = Block::Main;
  std::string gas;
  int segment = -1;
};
struct BlockFinished {
  std::string run_id;
  Block block = Block::Main;
  bool ok = true;
  std::string gas;
  int segment = -1;
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

// One thing the engine did to the spectrometer, in order. `what`: move,
// settle, table, optics, deflect, protect, integration, collect, step,
// prepare, pump_out. `detail`: its target or value ("Ar40@H1", "mass
// 34.2@H1", "3", "CDD on", "peak_center Ar40@H1"). `t`: seconds since the
// measurement started. `segment`, `position`: indices within the gas, -1
// when the entry belongs to none.
struct TraceEntry {
  std::string gas;
  int segment = -1;
  int position = -1;
  std::string what;
  std::string detail;
  double t = 0;
  friend bool operator==(const TraceEntry&, const TraceEntry&) = default;
};

// A detector's timing from spectrometer.toml (measurement plan v2 spec, 3.6;
// detector_timing() in adapters.hpp). A position settles for the largest of
// its own settle_s, the settle_after_large_beam_s of every detector it
// collects (active or not) whose last reading of gas (signal, sniff, whiff;
// a baseline reading leaves it as it was) exceeded large_beam_threshold, and,
// when it puts a collector in another mode, the mode_switch_s of the two
// modes. `collector` empty: the detector is its own collector.
struct DetectorTiming {
  std::optional<double> large_beam_threshold;
  double settle_after_large_beam_s = 0;
  std::string collector;
  double mode_switch_s = 0;
};

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
  // By detector name; a detector not in it has no large-beam or mode timing.
  std::map<std::string, DetectorTiming> timing = {};
  // spectrometer.toml's reference_detector: positions what the plan's own
  // reference would when the plan names none (plan::position_target).
  std::string reference = {};
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
  std::vector<TraceEntry> trace;          // what the engine did, in order
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
  // Live `cycles` (spec 6.3): the running segment runs `cycles` (at least
  // the cycle in progress) and later segments as planned. Nothing between
  // segments.
  void set_cycles(int cycles);

  const collect::Collector& collector() const noexcept { return collector_; }

 private:
  class Api;
  enum class Stop { None, Terminate, Cancel, Abort };

  // Block steps. A non-ok result is a Failed measurement; a Stop is read from
  // stop_ after each step.
  Result<void> run_blocks();
  // A segment of the running gas: a step, or `cycles` x its positions.
  Result<void> run_segment(std::size_t segment);
  Result<void> run_step(std::size_t segment);
  Result<void> peak_center(const plan::PeakCenterStep& step);
  Result<void> peak_center_on(PeakCenterRequest request, double min_signal);  // one detector
  Result<void> set_step(const plan::SetStep& set, int segment);
  // A deflection that stands until the run ends (a set step's, a hook's).
  Result<void> set_standing_deflection(const std::string& det, double value, int segment);
  Result<void> equilibrate();
  Result<void> whiff(std::size_t segment);
  Result<void> call_hook(std::string_view entry, const scripting::ValueMap& args = {});

  // The position executor: every collection of a segment goes through it.
  Result<collect::CollectStatus> run_position(const plan::Gas& gas, std::size_t segment, std::size_t position,
                                              const plan::Position& pos, int counts);
  // Everything run_position does before it collects: table, optics,
  // protect, deflections, move, settle, unprotect (nothing when unchanged).
  Result<void> enter_position(const plan::Segment& seg, const plan::Position& pos, int segment, int position);
  // Puts the magnet at `target` (nothing when nullopt or already there):
  // protect, move, settle, unprotect.
  Result<void> position_magnet(const std::optional<plan::At>& target, double settle_s,
                               const std::vector<std::string>& protect, int segment, int position);
  // Restores the deflections the engine changed, except those `keep` sets
  // (all of them when `keep` is null).
  Result<void> restore_deflections(const plan::Position* keep, int segment, int position);
  // The settle a move onto `pos` needs (DetectorTiming), and whether it puts
  // a collector in another mode.
  struct Settle {
    double s = 0;
    bool mode_change = false;
  };
  Settle settle_for(const plan::Position& pos) const;
  // Records which mode each collector of `pos` is in.
  void note_modes(const plan::Position& pos);
  // Large-beam state from one reading; a baseline reading changes nothing.
  void note_beams(std::string_view kind, const spectrometer::Reading& reading);
  Result<collect::CollectStatus> collect(Block block, collect::CollectionSpec spec, double integration_s,
                                         collect::ReadingHook hook);
  bool main_reading(int count, double t);
  // The block a segment of the running gas publishes (v1 names where they apply).
  Block block_of(std::size_t segment) const;
  // The label its collections are counted under: "main", "baseline.after", "segment[2]".
  std::string collection_label(std::size_t segment, Block block) const;
  void trace(int segment, int position, std::string what, std::string detail);
  const plan::Equilibration& equilibration() const;

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
  void rebuild_conditionals();
  void cleanup(bool abnormal);

  EngineContext ctx_;
  MeasurementInputs in_;
  EngineOptions options_;
  collect::Collector collector_;

  scripting::CancelToken* token_ = nullptr;
  std::unique_ptr<ConditionalEngine> conditionals_;
  MeasurementResult result_;
  const plan::Gas* gas_ = nullptr;           // the gas being run
  std::size_t main_ = 0;                     // its main segment
  plan::TimeZero time_zero_;                 // the main segment's, else the gas's
  Block block_ = Block::Main;                // the block running
  std::optional<plan::At> position_;         // where the magnet is, when known
  std::string table_;                        // field table selected ("" the configured one)
  std::set<std::string> unwritten_tables_;   // write_tables a failed peak center left unwritten
  std::string optics_;                       // optics preset applied ("" none yet)
  struct Deflected {
    double wanted = 0;    // what the plan asked for
    double original = 0;  // what the detector had before the engine changed it
  };
  std::map<std::string, Deflected> deflected_;      // deflections the engine changed, by detector
  std::map<std::string, Deflected> standing_;       // deflections a set step changed: restored at the end
  std::optional<double> step_integration_;          // a set step's integration_s, for the gas
  bool time_zero_by_step_ = false;                  // a time_zero step set it
  bool blanked_ = false;                            // a blank_beam step blanked the beam
  bool settle_due_ = false;                         // a restore moved a beam since the last settle
  // For the whole measurement, across segments and gases (DetectorTiming).
  std::set<std::string> large_beam_;                // detectors whose last gas reading was large
  std::map<std::string, std::string> modes_;        // collector -> the detector (mode) last used on it
  std::optional<pychron::Duration> integration_;    // of the last acquisition started
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
  // Live cycles of the running segment (set_cycles); guarded by cycles_mutex_.
  std::mutex cycles_mutex_;
  bool segment_running_ = false;
  int cycle_ = 0;                      // the cycle in progress
  std::optional<int> cycles_override_;
  int main_cycles_ = 0;                // the cycles main ran to (0: not yet run)
  std::optional<double> truncate_request_;  // count ratio for later collections
  std::atomic<bool> collecting_{false};
};

}  // namespace pychron::experiment::measurement
