#pragma once

// One automated run (experiment spec 3.1): the phases over a RunSpec.
//
//   Prepare       allocate the aliquot, load the plan and the run's merged
//                 conditionals, resolve scripts, begin_run on the persister
//   Extract       the extraction script on the script host; the extraction
//                 device is always ended and disabled afterwards
//   Measure       the MeasurementEngine (which owns equilibration); the
//                 post-equilibration script starts when the inlet closes
//   PostMeasure   the post-measurement script; pump time starts here
//   Save          AnalysisRecord -> SavePipeline (spool first)
//
// Control (RunControl, thread-safe):
//   cancel()     scripts raise ScriptCancelled; post-equilibration and
//                post-measurement still run; nothing is saved -> Cancelled
//   abort()      stops at once; no further scripts -> Aborted
//   truncate()   ends the current measurement block; the run saves as
//                Success with truncated = true
// A post-measurement failure still saves. A save failure ends
// Failed(save_error) with the record left in the spool; a record the persister
// did not take leaves the run as it was, flagged save_error (RunResult).
//
// Hooks let the executor coordinate overlapped runs: acquire/release of the
// extraction device and the spectrometer, and the overlap / pump-time
// signals.

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/devices/extraction/services.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/measurement/keep_trying.hpp"
#include "pychron/experiment/model/run_spec.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/plan/plan_library.hpp"
#include "pychron/experiment/record/types.hpp"
#include "pychron/experiment/run/state.hpp"
#include "pychron/reduction/arar.hpp"
#include "pychron/scripting/cancel_token.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/scripting/services.hpp"

namespace pychron::experiment::run {

struct SpectrometerInfo {
  std::string state_hash;   // SpectrometerState::hash_hex()
  std::string field_table;  // active table version
  double integration_s = 0;
};

// Everything a run touches. Optional pieces may be null:
//   scripts / resolver   required only when the run names a script
//   spectrometer         required when the run has a measurement plan
//   conditionals         null: only the plan's inline truncations apply
struct RunServices {
  const Clock* clock = nullptr;
  SignalBus* bus = nullptr;

  scripting::IScriptHost* scripts = nullptr;
  const scripting::IScriptResolver* resolver = nullptr;
  extraction::ExtractionServices line;
  // The lab's extraction devices by name. A run asks once, for its own
  // device (its spec's, else the queue's), and uses the answer as
  // line.device for its scripts and for ending the extraction. A null
  // result leaves the run without one. line.device, when set, wins.
  std::function<extraction::IExtractionDevice*(std::string_view name)> devices;
  scripting::IResourceService* resources = nullptr;

  measurement::ISpectrometerPort* spectrometer = nullptr;
  measurement::IValvePort* valves = nullptr;
  measurement::IPeakCenterPort* peak_center = nullptr;
  const MetricContext* instrument_metrics = nullptr;
  std::function<SpectrometerInfo()> spectrometer_info;
  measurement::EngineOptions engine;  // sleep injection, timeouts
  // spectrometer.toml's per-detector settle timing (measurement::detector_timing).
  std::map<std::string, measurement::DetectorTiming> detector_timing;
  // spectrometer.toml's reference_detector, for a plan that names no reference.
  std::string reference_detector;
  // spectrometer.toml's [interferences] (monitor, corrected): each gas's record carries them.
  std::vector<std::pair<std::string, std::string>> interferences;
  // How long a step of the safe state keeps trying (executor.toml
  // [links] critical_window_s; the spec's 10 min by default).
  pychron::Duration critical_window = measurement::kCriticalWindow;

  const plan::PlanLibrary* plans = nullptr;
  // Where a plan's files are (a baseline modifier's model CSV, spec 3.5).
  std::filesystem::path plan_dir;
  const ConditionalLibrary* conditionals = nullptr;

  persist::AliquotAllocator* aliquots = nullptr;
  persist::IAnalysisPersister* persister = nullptr;
  persist::SavePipeline* save = nullptr;

  record::InstrumentMeta instrument;
  std::optional<reduction::ArArConstants> arar;
  std::map<std::string, double> icfactors;

  std::function<std::string()> make_uuid;   // default: random v4
  std::function<std::string()> timestamp;   // default: system clock, ISO-8601 UTC
};

// Coordination with the executor; every member is optional.
struct RunHooks {
  std::function<bool()> acquire_extraction;  // false: cancelled while waiting
  std::function<void()> release_extraction;
  std::function<bool()> acquire_spectrometer;
  std::function<void()> release_spectrometer;
  std::function<void()> on_overlap_ready;     // the inlet closed
  std::function<void()> on_pump_time_started;
};

class RunControl {
 public:
  void cancel();
  void abort();
  void truncate(bool quick = false);
  void set_counts(int counts);
  void set_cycles(int cycles);  // the running segment's (MeasurementEngine::set_cycles)

  scripting::CancelToken& token() noexcept { return token_; }
  bool requested() const noexcept { return token_.requested(); }

 private:
  friend class Run;
  void attach(measurement::MeasurementEngine* engine);

  scripting::CancelToken token_;
  std::mutex mutex_;
  measurement::MeasurementEngine* engine_ = nullptr;
  std::optional<bool> pending_truncate_;  // quick?
};

// Something a run said: a script's info() line, what a hole move did
// ("hole 3: centered, ..."), a failure the run carried on past. Published on
// the bus as it is said, from whichever thread said it; the same lines are
// RunResult::messages and the record's "note" events.
struct RunNote {
  std::string run_id;
  std::size_t row = 0;  // the run's row in its queue (Run's `row`)
  std::string text;
  TimePoint ts{};
};

// How the note of a run begins whose record was spooled and not taken by the
// persister; the persister's own words follow. Whoever reports such a run
// (the notification, the queue table) finds its reason by this.
inline constexpr std::string_view kNotSavedNote = "not saved to the data store, kept in the spool: ";

// What the safe state (spec 4.7) could not do; empty: the line is safe.
struct SafeStateResult {
  std::vector<std::string> failed_steps;
};

// How long a step of the safe state keeps trying before it alarms, and how
// long in all (the critical window, spec 4.4; from executor.toml later).
inline constexpr pychron::Duration kSafeStateAlarmAfter = measurement::kCriticalAlarmAfter;
inline constexpr pychron::Duration kSafeStateWindow = measurement::kCriticalWindow;

struct RunResult {
  RunState state = RunState::Pending;
  bool truncated = false;
  // Extraction began: the sample is spent from here, and a run that fails
  // after it saves what it has, status failed (spec 4.5).
  bool past_no_return = false;
  // A step of the safe state (spec 4.7) failed after its window: the line may
  // not be safe. unsafe_steps names them, in the order they ran.
  bool unsafe = false;
  std::vector<std::string> unsafe_steps;
  // The record is not where finished runs go. Either it could not be spooled
  // or finalized (the run is then Failed), or it is in the spool and the
  // persister did not take it (the run keeps its state: a measured run is
  // still Success, and a note that begins with kNotSavedNote says why).
  bool save_error = false;
  std::optional<Error> error;
  std::string uuid;
  int aliquot = 0;
  std::string step;
  std::optional<record::AnalysisRecord> record;   // as saved (or spooled on save_error)
  measurement::MeasurementResult measurement;
  std::vector<std::string> messages;              // every RunNote's text, in order
  std::vector<RunStateChanged> history;
};

class Run {
 public:
  // `row` is the run's row in its queue; it only labels what the run
  // publishes (RunNote).
  Run(RunSpec spec, const QueueSpec& queue, RunServices services, RunHooks hooks = {}, int run_index = 0,
      std::size_t row = 0);

  // Runs every phase on the calling thread.
  RunResult execute(RunControl& control);

  // Puts the line in a safe state after a failed run (spec 4.7): the
  // extraction device ended and disabled, the plan's inlet closed, the
  // detectors the plan protects unprotected. Each step keeps trying through
  // transient failures (backoff on the run's clock) for
  // RunServices::critical_window (kSafeStateWindow by default),
  // with a warning Alarm at kSafeStateAlarmAfter and a critical one when it
  // gives up; a definite error gives up at once. Steps after a failed one
  // still run. Idempotent: every target is absolute. execute() calls it;
  // calling it again is allowed.
  [[nodiscard]] SafeStateResult make_safe();

  const RunSpec& spec() const noexcept { return spec_; }
  RunState state() const { return sm_.state(); }
  const std::string& id() const noexcept { return run_id_; }
  // Gives the run its aliquot (and step) now, as prepare() would: the executor
  // calls it before it says the run started, so the run's name is known from
  // its first record. Calling it is optional; prepare() then does it. An
  // allocation that fails is reported by prepare(), which fails the run.
  void assign_aliquot();
  // The run's name, `<identifier>-<aliquot:02><step>` (format_runid): what its
  // log records carry as run=. The identifier alone until an aliquot is given.
  const std::string& name() const noexcept { return name_; }

 private:
  Result<void> prepare();
  Result<void> extract(RunControl& control);
  Result<void> measure(RunControl& control);
  // The measurement engine, built once: by extract() when the plan has a
  // during_extraction phase, else by measure().
  void build_engine(RunControl& control);
  bool has_extraction_phase() const;
  bool has_prepare() const;  // a gas of the plan has a prepare script (spec 11.2)
  void release_held_extraction();

  // A gas's prepare script (spec 11.2), run as an extraction script: the
  // engine's prepare port.
  class PrepareScripts final : public measurement::IPrepareScriptPort {
   public:
    explicit PrepareScripts(Run& run) : run_(run) {}
    Result<measurement::PrepareReport> run(const measurement::PrepareRequest& request,
                                           scripting::CancelToken& token) override;

   private:
    Run& run_;
  };
  Result<void> post_measure(RunControl& control);
  // The record as far as the run got. `failure`: the run failed after
  // extraction began, and this is why (status failed); otherwise complete,
  // or truncated when it was.
  Result<void> save(const std::optional<Error>& failure = std::nullopt);

  Result<scripting::Script> resolve(const std::string& name, scripting::ScriptKind kind) const;
  Result<void> run_script(const scripting::Script& script, scripting::ScriptKind kind, RunControl& control,
                          std::function<void()> on_pump_time_start = {});
  scripting::ScriptContext script_context() const;
  void end_extraction();
  // `op` until it succeeds, a definite error, or the critical window (above).
  Result<void> keep_trying(const std::string& step, const std::function<Result<void>()>& op);
  void alarm(AlarmSeverity severity, std::string message);
  void record_cryo();
  void advance(RunEvent e, std::string reason = {});
  void note(std::string message);  // thread-safe: scripts log from their own threads
  void finish(RunControl& control, Result<void> phase_result, RunEvent failure_event);

  Logger log_ = process_logger("experiment.run");  // taken when the run is made
  RunSpec spec_;
  const QueueSpec& queue_;
  RunServices s_;
  extraction::IExtractionDevice* device_ = nullptr;  // this run's; bound in prepare()
  RunHooks hooks_;
  int run_index_;
  std::size_t row_;
  std::string run_id_;
  std::string name_;
  bool assigned_ = false;              // assign_aliquot() ran and prepare() has not used it yet
  std::optional<Error> assign_error_;  // why it could not give an aliquot
  RunStateMachine sm_;
  RunResult result_;

  // Prepared.
  std::optional<plan::LoadedPlan> plan_;
  ConditionalSet conditionals_;
  std::optional<scripting::Script> extraction_, post_eq_, post_meas_;
  std::map<std::string, record::ScriptRef> script_refs_;
  record::ExtractionActuals actuals_;
  std::string timestamp_;
  std::mutex messages_mutex_;
  std::vector<TimePoint> message_times_;  // one per result_.messages

  // The measurement (build_engine).
  std::unique_ptr<measurement::MeasurementEngine> engine_;
  bool spectrometer_held_ = false;  // hooks_.acquire_spectrometer answered, not yet released
  // The post-equilibration script beside the measurement: said done under
  // post_eq_mutex_, through the clock.
  std::thread post_eq_thread_;
  std::mutex post_eq_mutex_;
  std::condition_variable post_eq_cv_;
  bool post_eq_done_ = false;
  // The during_extraction phase beside the extraction script (spec 5.4), on
  // its own token, cancelled with the run's or when the extraction fails;
  // said done under phase_mutex_, through the clock.
  std::thread phase_thread_;
  std::mutex phase_mutex_;
  std::condition_variable phase_cv_;
  bool phase_done_ = false;
  std::optional<Error> phase_error_;
  scripting::CancelToken phase_token_;
  bool extraction_held_ = false;  // a staged plan keeps the line to the end of its measurement
  PrepareScripts prepare_scripts_{*this};
};

}  // namespace pychron::experiment::run
