#include "main_window.hpp"

#include <QStatusBar>

namespace pychron::ui {

MainWindow::MainWindow(systems::ExtractionLine& line, QWidget* parent)
    : QMainWindow(parent),
      bridge_(line),
      canvas_(new CanvasView(bridge_, this)),
      log_(new LogDock(this)),
      alarms_(new AlarmDock(this)),
      health_(new HealthBar(this)) {
  setWindowTitle(QStringLiteral("pychron — %1").arg(QString::fromStdString(line.config().system.name)));
  setCentralWidget(canvas_);
  addDockWidget(Qt::BottomDockWidgetArea, log_);
  addDockWidget(Qt::RightDockWidgetArea, alarms_);
  statusBar()->addPermanentWidget(health_, 1);

  std::vector<std::string> transports;
  for (const auto& [name, cfg] : line.config().transports) {
    transports.push_back(name);
  }
  health_->seed(transports);

  for (const auto& w : line.warnings()) {
    log_->append_line(QStringLiteral("WARN [canvas] ") + QString::fromStdString(config::to_string(w)));
  }

  connect(&bridge_, &CoreBridge::logLine, log_, &LogDock::append_log);
  connect(&bridge_, &CoreBridge::alarm, alarms_, &AlarmDock::add_alarm);
  connect(&bridge_, &CoreBridge::transportHealth, health_, &HealthBar::update_health);
  connect(&bridge_, &CoreBridge::actuationFinished, log_, [this](const QString& name, const Result<void>& r) {
    if (!r) {
      log_->append_line(QStringLiteral("ERROR [ui] %1 rejected: %2").arg(name, QString::fromStdString(to_string(r.error()))));
    }
  });
}

}  // namespace pychron::ui
