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
// Failed(save_error) with the record left in the spool.
//
// Hooks let the executor coordinate overlapped runs: acquire/release of the
// extraction device and the spectrometer, and the overlap / pump-time
// signals.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/devices/extraction/services.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/measurement/engine.hpp"
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

  const plan::PlanLibrary* plans = nullptr;
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

struct RunResult {
  RunState state = RunState::Pending;
  bool truncated = false;
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

  const RunSpec& spec() const noexcept { return spec_; }
  RunState state() const { return sm_.state(); }
  const std::string& id() const noexcept { return run_id_; }

 private:
  Result<void> prepare();
  Result<void> extract(RunControl& control);
  Result<void> measure(RunControl& control);
  Result<void> post_measure(RunControl& control);
  Result<void> save();

  Result<scripting::Script> resolve(const std::string& name, scripting::ScriptKind kind) const;
  Result<void> run_script(const scripting::Script& script, scripting::ScriptKind kind, RunControl& control,
                          std::function<void()> on_pump_time_start = {});
  scripting::ScriptContext script_context() const;
  void end_extraction();
  void note(std::string message);  // thread-safe: scripts log from their own threads
  void finish(RunControl& control, Result<void> phase_result, RunEvent failure_event);

  RunSpec spec_;
  const QueueSpec& queue_;
  RunServices s_;
  extraction::IExtractionDevice* device_ = nullptr;  // this run's; bound in prepare()
  RunHooks hooks_;
  int run_index_;
  std::size_t row_;
  std::string run_id_;
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
};

}  // namespace pychron::experiment::run
