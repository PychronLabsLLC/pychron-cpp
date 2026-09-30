#pragma once

// M1 status/control window (spec section 10.3): canvas in the centre, log and
// alarm docks, per-transport health chips in the status bar. Owns the
// CoreBridge; the ExtractionLine must outlive the window.

#include <QMainWindow>

#include "alarm_dock.hpp"
#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "health_bar.hpp"
#include "log_dock.hpp"

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

 private:
  CoreBridge bridge_;
  CanvasView* canvas_;
  LogDock* log_;
  AlarmDock* alarms_;
  HealthBar* health_;
};

}  // namespace pychron::ui
