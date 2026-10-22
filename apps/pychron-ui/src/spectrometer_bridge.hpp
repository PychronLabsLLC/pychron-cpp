#pragma once

// SpectrometerBridge (spectrometer-window design section 5.2): the QObject
// between the Qt-free spectrometer core and the spectrometer window. Same
// rules as CoreBridge.
//
// State out: IntensityReading, MagnetMoved, DetectorState and ScanStatus
// events, whatever thread published them, are marshalled to the bridge's
// (main) thread with a queued call, folded into State, then re-emitted as Qt
// signals. Readings are queued under a mutex on the publishing thread and
// delivered by one pending queued call, so a burst arrives as a single
// in-order batch and a slow repaint never backs up the bus.
//
// Commands in: start_scan / stop_scan / set_integration / position return
// immediately; the blocking core call runs on one executor QThread, in the
// order issued, and its Result is posted back as commandFinished. Destruction
// finishes the command in flight and drops queued ones, except that a stop
// asked for after the last start always happens.
//
// Field tables: replace_table and restore_table_version run on the same
// executor and report through tableSaved / tableRestored. FieldTableChanged
// (from them, from a peak center's save, from anything else) is relayed; the
// bridge re-reads its copy of the active table on the executor, since the
// core's table lock can be held across a settling move, then emits
// fieldTableChanged with the copy already current.
//
// Peak center (spectrometer peak center design, section 4; run from the Peak
// Center window): a run goes on a thread of its own, not the executor, so
// stop_scan and the rest stay free meanwhile. The thread takes part in the
// spectrometer's clock and is started and joined by the rules of AGENTS.md
// "Time". Its PeakCenterTryStarted and PeakCenterPoint events are relayed
// only while the bridge's own run is going (a plan's are not), in one
// ordered queue under the gate, delivered by one pending queued call as
// readings are; the result comes back as peakCenterFinished after the last
// of them. The destructor cancels a run and waits for its thread.
//
// No core object ever holds a QObject*: the bus handlers reach the bridge only
// through a shared Gate the destructor closes before the bridge goes away.

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include "pychron/core/error.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/systems/jobs/cancel.hpp"
#include "pychron/systems/jobs/peak_center.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"
#include "strip_chart_model.hpp"

namespace pychron::ui {

// Letters, digits, '_' and '-' (a peak_center.toml table name written bare).
bool valid_set_name(const QString& name);

class SpectrometerBridge : public QObject {
  Q_OBJECT

 public:
  // Main-thread mirror of what the core has told the UI.
  struct State {
    std::optional<double> magnet_native;      // unknown until start_scan's read or the first move
    std::optional<double> mass_on_reference;  // amu
    std::map<std::string, std::string> isotopes;  // detector -> isotope
    spectrometer::ScanStatus scan;
  };

  // `spec`, `scan` and `bus` must outlive the bridge. The detector list and
  // the active field table are read once, here.
  SpectrometerBridge(spectrometer::Spectrometer& spec, spectrometer::ScanService& scan, SignalBus& bus,
                     QObject* parent = nullptr);
  ~SpectrometerBridge() override;
  SpectrometerBridge(const SpectrometerBridge&) = delete;
  SpectrometerBridge& operator=(const SpectrometerBridge&) = delete;

  const State& state() const noexcept { return state_; }

  // Immutable description; safe to read from the main thread at any time.
  QString name() const;
  // Config order; a detector without a configured colour gets the palette's.
  std::vector<DetectorSeries> detectors() const;
  // Isotopes of the field table's points that have a value on `detector`,
  // table order. Empty for a detector the table does not know.
  QStringList isotopes_for(const QString& detector) const;
  std::optional<double> mass_of(const QString& isotope) const;  // amu; nullopt when unknown
  // Mass (amu) the reference detector sees with `isotope` centered on
  // `detector` (empty: the reference), from the field table alone: the
  // deflection and HV corrections are left out, since applying them reads
  // hardware. nullopt when the table cannot say.
  std::optional<double> mass_on_reference_for(const QString& isotope, const QString& detector) const;
  QString reference_detector() const;
  double default_integration_s() const;  // config integration_time_s

  // Non-blocking; each reports once through commandFinished as "start",
  // "stop", "integration" or "position". start_scan also reads the magnet
  // position to seed State (announced through magnetRead); a failed read
  // leaves it unset and does not fail the start. The restart after a peak
  // center is reported as an internal "start" too. While a peak center runs,
  // start_scan and position are refused (Config "a peak center is running",
  // through commandFinished) and change nothing; stop_scan is not.
  void start_scan(double integration_s);
  void stop_scan();
  void set_integration(double seconds);
  // Centers `isotope` on `detector` and, on success, records it as that
  // detector's isotope (announced through detectorChanged).
  void position(const QString& isotope, const QString& detector);

  // Blocks until every queued command has finished.
  void drain();

  // ---- field tables (field table editor spec, section 5) ----
  // Immutable description; safe to read from the main thread at any time.
  std::filesystem::path data_root() const;
  const spectrometer::cfg::SpectrometerConfig& config() const;
  const spectrometer::MolecularWeights& weights() const;
  // The table that was active when the bridge was built (config's).
  QString active_table() const;
  // The first holder of a TableHold still out; a straight call, since holds
  // have their own small lock in the core.
  std::optional<QString> table_held() const;
  // The bridge's copy of the active table, current as of the last
  // fieldTableChanged; an empty table (no points) for any other name.
  spectrometer::FieldTable table_copy(const QString& name) const;
  // Non-blocking. Spectrometer::replace_table stamped from the
  // spectrometer's wall clock, reported once through tableSaved; an empty
  // `base_version` skips the conflict check.
  void replace_table(const QString& name, spectrometer::FieldTable table, const QString& base_version);
  // Non-blocking. Spectrometer::restore_table_version, reported once
  // through tableRestored.
  void restore_table_version(const QString& name, const QString& version);

  // ---- peak center (spectrometer peak center design, section 4) ----
  // Non-blocking: runs jobs::run_peak_center on a thread of its own, with
  // update_table forced off (the window applies a result only when asked),
  // and reports once through peakCenterFinished. Refused, with Config "a
  // peak center is running" through peakCenterFinished, while one is; an
  // accepted start says so through peakCenterStarted before it returns.
  void peak_center(jobs::PeakCenterConfig config);
  // What a cancelled run does with the scan it stopped: puts it back, as any
  // run's end does, or leaves it stopped (for a window that is closing,
  // which would stop it again at once).
  enum class ScanAfter { Resume, Stopped };
  // No-op when none is running; the run then reports Cancelled. It is not
  // stopped at once: the runner checks between steps (an acquisition under
  // way is ended through the token's hook), and a settle or other clock wait
  // runs to its end first.
  void cancel_peak_center(ScanAfter after = ScanAfter::Resume);
  // Cancels a run (ScanAfter::Stopped) and waits for its thread, then
  // delivers its finish at once, so peak_center_running() is false on
  // return. For a caller about to delete what shows the run; asks nothing.
  // No-op when none.
  void cancel_peak_center_and_wait();
  // From the start until its peakCenterFinished has been delivered.
  bool peak_center_running() const noexcept { return peak_center_running_; }
  // Non-blocking, on the executor: jobs::write_peak_center_to_table into the
  // active table, saved. Reported once through peakCenterApplied (a failed
  // save is not a failure: it comes first as a peakCenterNote).
  void apply_peak_center(const QString& detector, const QString& isotope, double table_value, bool propagate);
  // Non-blocking, on the executor: writes `config` as table `[name]` of
  // `file` (peak_center.toml), keeping the file's comments and other sets,
  // then reads the whole file again. A name that valid_set_name refuses is
  // reported as Config before anything is written. Reported once through
  // peakCenterSetSaved.
  void save_peak_center_set(std::filesystem::path file, const QString& name, jobs::PeakCenterConfig config);

 signals:
  void readings(const std::vector<pychron::spectrometer::IntensityReading>& batch);
  void magnetMoved(const pychron::spectrometer::MagnetMoved& event);
  void magnetRead(double native, std::optional<double> mass);  // mass on the reference detector
  void detectorChanged(const pychron::spectrometer::DetectorState& event);
  void scanStatus(const pychron::spectrometer::ScanStatus& status);
  void commandFinished(const QString& what, const pychron::Result<void>& result);
  // A table got a new current version; table_copy and isotopes_for already
  // reflect it.
  void fieldTableChanged(const QString& name, const QString& version);
  void tableSaved(const QString& name, const pychron::Result<std::string>& result);  // the new version
  void tableRestored(const QString& name, const QString& version, const pychron::Result<void>& result);
  // peak_center() accepted a run (not a refused start): emitted from inside
  // that call, on the main thread, with peak_center_running() already true.
  void peakCenterStarted();
  // Relayed from the bus while the bridge's own run is going, in the order
  // published: a try's start before its points.
  void peakCenterTryStarted(const pychron::jobs::PeakCenterTryStarted& event);
  void peakCenterPoint(const pychron::jobs::PeakCenterPoint& point);
  // The run's result, after its last point; also a refused start.
  void peakCenterFinished(const pychron::Result<pychron::jobs::PeakCenterResult>& result);
  void peakCenterApplied(const pychron::Result<void>& result);
  // What went wrong without failing the apply, e.g. "field table not saved: <why>".
  void peakCenterNote(const QString& note);
  void peakCenterSetSaved(const QString& name, const pychron::Result<void>& result);
  // Internal: a job waits in the Gate for the executor, a call for the main
  // thread. Not for the window.
  void jobQueued();
  void deliveryQueued();

 private:
  struct Gate;
  using Job = std::function<void(Gate&)>;
  // What the bridge and a run's thread share. The thread's last act, while
  // still a participant, is to set `done` and notify through the clock.
  struct PeakCenterRun {
    std::mutex mutex;
    std::condition_variable cv;  // waited on and notified through the spectrometer's clock
    bool done = false;
    jobs::CancelToken cancel;
  };

  template <class E>
  void relay(void (SpectrometerBridge::*apply)(const E&));
  // Runs `command` on the executor and posts its Result back as `what`.
  template <class Command>
  void run(const char* what, Command command);
  // Runs `work` on the executor; its result is handed to `deliver` on the
  // main thread unless the bridge is gone by then.
  template <class Work, class Deliver>
  void post(Work work, Deliver deliver);
  // Reports `what` refused (Config "a peak center is running"), queued as a
  // command's result would be.
  void refuse_during_peak_center(const char* what);

  // Queues `job` for the executor.
  void enqueue(Job job);
  // Runs the oldest call waiting for the main thread.
  void deliver_next();
  void flush_readings();
  void on_magnet(const spectrometer::MagnetMoved& e);
  void on_magnet_read(double native, std::optional<double> mass);
  void on_detector(const spectrometer::DetectorState& e);
  void on_scan(const spectrometer::ScanStatus& e);
  void on_table_changed(const spectrometer::FieldTableChanged& e);
  void start_peak_center_thread(jobs::PeakCenterConfig config);
  void flush_peak_center_events();
  void on_peak_center_finished(const Result<jobs::PeakCenterResult>& result);
  // Waits through the clock for the run's thread to say it is done, then
  // joins it. Nothing when there is none.
  void join_peak_center();

  spectrometer::Spectrometer& spec_;
  spectrometer::ScanService& scan_;
  SignalBus& bus_;
  std::string active_table_;        // active table at construction
  spectrometer::FieldTable table_;  // copy of it, re-read on FieldTableChanged
  State state_;
  std::shared_ptr<Gate> gate_;
  std::vector<SignalBus::Subscription> subscriptions_;
  QThread executor_;
  QObject* worker_ = nullptr;  // lives on executor_
  bool stop_requested_ = false;  // stop_scan since the last start_scan (main thread)
  std::unique_ptr<PeakCenterRun> peak_center_run_;  // until the run's thread is joined
  std::thread peak_center_thread_;
  bool peak_center_running_ = false;  // main thread
  // Integration (s) of the scan the run stopped; set while it must be restarted.
  std::optional<double> resume_integration_s_;
  // Scan the operator asked for (start_scan, set_integration), unset after
  // stop_scan, after a start that failed, and after a scan that stopped with
  // no start of ours in flight; main thread.
  std::optional<double> scan_intent_;
  std::uint64_t start_generation_ = 0;  // each start_scan and stop_scan; main thread
  int starts_in_flight_ = 0;            // start_scan not yet reported; main thread
};

}  // namespace pychron::ui
