#pragma once

// M1 status/control window (spec section 10.3): canvas in the centre, log and
// alarm docks, per-transport health chips in the status bar. Owns the
// CoreBridge; the ExtractionLine must outlive the window.

#include <functional>
#include <memory>

#include <QAction>
#include <QMainWindow>
#include <QSettings>

#include "alarm_dock.hpp"
#include "canvas_view.hpp"
#include "core_bridge.hpp"
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

 protected:
  void closeEvent(QCloseEvent* event) override;  // closes the spectrometer window first

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
};

}  // namespace pychron::ui
