#pragma once

// LaserBridge (laser window design, section 5): the QObject between one
// laser system and its window. Same rules as SpectrometerBridge: no core
// object holds a QObject*, the core's blocking calls never run on the main
// thread, and results come back as signals on it.
//
// One worker thread per bridge. Between commands, and while one waits for a
// motion to end, it takes the system's snapshot (and its camera's view) every
// `publish` and posts them to the main thread. Commands run in the order
// issued. One that starts a motion (a move, a centring, a pattern) waits for
// it there, polling every `poll`: nothing is left half centred because
// nobody was asking.
//
// Who may drive: every driving command takes the lab's Manual lease for as
// long as it runs, and fails at once while a queue holds the lasers ("a queue
// is running"). The window is then watch-only.
//
// Stopping: stop_stage, stop_beam and stop_pattern drop every command still
// queued (each reports Cancelled), cancel the one in flight, then act.
// emergency_stop does the same, needs no lease, stops the laser system
// (which latches, and is latched at once on the calling thread) and then
// asks for the queue to be aborted.
//
// A bridge that goes while its laser fires closes the beam and disables the
// laser, unless a queue is driving: nothing would be left to do it.

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>

#include "pychron/core/error.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/lab/lasers.hpp"
#include "pychron/laser/laser_system.hpp"

namespace pychron::ui {

struct LaserBridgeDeps {
  experiment::lab::Lasers& lasers;  // must outlive the bridge, as must `lab`
  const experiment::lab::Lab& lab;
  std::string device;               // one of lasers.names()
  std::function<void()> abort_queue;  // called (on the worker) by an emergency stop; may be empty
  std::chrono::milliseconds poll{50};
  std::chrono::milliseconds publish{250};
};

class LaserBridge : public QObject {
  Q_OBJECT

 public:
  explicit LaserBridge(LaserBridgeDeps deps, QObject* parent = nullptr);
  ~LaserBridge() override;
  LaserBridge(const LaserBridge&) = delete;
  LaserBridge& operator=(const LaserBridge&) = delete;

  // Fixed for the bridge's life; main thread.
  QString device() const { return QString::fromStdString(deps_.device); }
  QStringList trays() const;
  const laser::TrayMap* tray_map(const QString& name) const;  // null: no such tray
  const laser::PatternLibrary& patterns() const { return deps_.lab.patterns; }
  bool has_camera() const;

  // Read from disk, as they are now; main thread.
  std::vector<laser::CalibrationPoint> calibration_points(const QString& tray) const;
  laser::CalibrationStatus calibration(const QString& tray) const;
  QStringList corrected_holes() const;  // of the current tray, as the system holds them

  // The last snapshot delivered; main thread.
  const laser::LaserSnapshot& state() const noexcept { return state_; }
  bool watch_only() const noexcept { return watch_only_; }  // a queue drives

  // Commands. Each returns at once and reports once through commandFinished
  // under its own name ("go_to", "jog", ...).
  void go_to(const QString& hole, bool autocenter);
  void jog(double dx, double dy, double dz);  // mm, from where the stage is
  void stop_stage();
  void set_tray(const QString& tray);
  void enable(bool on);
  void fire(double percent);        // sets the output, then opens the beam
  void set_output(double percent);
  void stop_beam();                 // output 0, beam closed
  void run_pattern(const QString& name);
  void stop_pattern();
  // A calibration point for `hole` of the current tray, at where the stage
  // is now. Points that do not solve are refused and the file is left.
  void add_calibration_point(const QString& hole);
  void remove_calibration_point(const QString& hole);
  void clear_calibration();
  void emergency_stop();
  void reset_stop();  // refused while a queue still holds the lasers

  // Blocks until every command issued has finished; their signals are still
  // to be delivered by the event loop.
  void drain();

 signals:
  void snapshot(const pychron::laser::LaserSnapshot& state);
  void view(const pychron::laser::CameraView& seen);
  void viewFailed(const QString& why);
  void commandFinished(const QString& what, const pychron::Result<void>& result);
  void driverChanged(bool watch_only);
  void calibrationChanged();

 private:
  struct Gate;
  struct Worker;
  using Action = std::function<Result<void>()>;

  // Queues `action` under `what`. `driving`: it needs the Manual lease.
  void submit(const char* what, Action action, bool driving = true);
  // Drops what is queued, cancels what runs, then queues `action`.
  void interrupt(const char* what, Action action, bool driving);
  void loop();
  void publish_now();
  // Polls `busy` until it says false, fails, or the command is cancelled
  // (then `halt` is called and Cancelled returned).
  Result<void> wait(const std::function<Result<bool>()>& busy, const std::function<void()>& halt);
  // On the worker: post to the main thread, if the bridge is still there.
  template <class F>
  void post(F&& f);
  void on_snapshot(const laser::LaserSnapshot& state, bool watch_only);
  Result<void> change_calibration(const std::function<void(std::vector<laser::CalibrationPoint>&, double, double)>& edit);

  LaserBridgeDeps deps_;
  laser::LaserSystem& system_;
  laser::LaserSnapshot state_;
  bool watch_only_ = false;
  std::shared_ptr<Gate> gate_;
  std::unique_ptr<Worker> worker_;
  std::thread thread_;
};

}  // namespace pychron::ui
