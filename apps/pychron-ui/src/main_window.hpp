#pragma once

// M1 status/control window (spec section 10.3): canvas in the centre, log and
// alarm docks, per-transport health chips in the status bar. Owns the
// CoreBridge; the ExtractionLine must outlive the window.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <QAction>
#include <QMainWindow>
#include <QSettings>

#include "alarm_dock.hpp"
#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "experiment_window.hpp"
#include "health_bar.hpp"
#include "log_dock.hpp"
#include "spectrometer_window.hpp"

namespace pychron::ui {

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(systems::ExtractionLine& line, QWidget* parent = nullptr);

  CoreBridge& bridge() noexcept { return bridge_; }
  CanvasView* canvas_view() const noexcept { return canvas_; }
  LogDock* log_dock() const noexcept { return log_; }
  AlarmDock* alarm_dock() const noexcept { return alarms_; }
  HealthBar* health_bar() const noexcept { return health_; }

  // Enables Window > Spectrometer for `bridge` (null disables it and closes
  // the window). The bridge must outlive the main window or be cleared first.
  // `settings` makes the window's QSettings (default: the application's);
  // tests pass a temp file.
  void set_spectrometer(SpectrometerBridge* bridge, bool simulation,
                        std::function<std::unique_ptr<QSettings>()> settings = {});
  QAction* spectrometer_action() const noexcept { return spectrometer_action_; }
  // Null until the action is first triggered.
  SpectrometerWindow* spectrometer_window() const noexcept { return spectrometer_window_; }

  // Enables Window > Experiment for `bridge` (null disables it and closes the
  // window). `queue` is opened the first time the window shows. Same
  // lifetime and settings rules as set_spectrometer.
  void set_experiment(ExperimentBridge* bridge, bool simulation, std::optional<std::filesystem::path> queue = {},
                      std::function<std::unique_ptr<QSettings>()> settings = {});
  QAction* experiment_action() const noexcept { return experiment_action_; }
  ExperimentWindow* experiment_window() const noexcept { return experiment_window_; }

 protected:
  // Closes the experiment window (which may refuse, keeping everything open)
  // and the spectrometer window first.
  void closeEvent(QCloseEvent* event) override;

 private:
  CoreBridge bridge_;
  CanvasView* canvas_;
  LogDock* log_;
  AlarmDock* alarms_;
  HealthBar* health_;
  QAction* spectrometer_action_;
  SpectrometerBridge* spectrometer_ = nullptr;
  bool simulation_ = false;
  std::function<std::unique_ptr<QSettings>()> settings_factory_;
  SpectrometerWindow* spectrometer_window_ = nullptr;
  QAction* experiment_action_;
  ExperimentBridge* experiment_ = nullptr;
  bool experiment_simulation_ = false;
  std::optional<std::filesystem::path> experiment_queue_;
  std::function<std::unique_ptr<QSettings>()> experiment_settings_;
  ExperimentWindow* experiment_window_ = nullptr;
};

}  // namespace pychron::ui
