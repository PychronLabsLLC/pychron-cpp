#include "main_window.hpp"

#include "figure_window.hpp"
#include "reference_fit_window.hpp"
#include "recall_window.hpp"

#include <utility>

#include <QCloseEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>

namespace pychron::ui {

MainWindow::MainWindow(systems::ExtractionLine& line, QWidget* parent)
    : QMainWindow(parent),
      bridge_(line),
      canvas_(new CanvasView(bridge_, this)),
      log_(new LogDock(this)),
      alarms_(new AlarmDock(this)),
      health_(new HealthBar(this)),
      spectrometer_action_(new QAction(QStringLiteral("Spectrometer"), this)),
      experiment_action_(new QAction(QStringLiteral("Experiment"), this)),
      data_action_(new QAction(QStringLiteral("Data"), this)) {
  setWindowTitle(QStringLiteral("pychron — %1").arg(QString::fromStdString(line.config().system.name)));
  setCentralWidget(canvas_);
  addDockWidget(Qt::BottomDockWidgetArea, log_);
  addDockWidget(Qt::RightDockWidgetArea, alarms_);
  statusBar()->addPermanentWidget(health_, 1);

  spectrometer_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
  spectrometer_action_->setEnabled(false);
  experiment_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
  experiment_action_->setEnabled(false);
  QMenu* window_menu = menuBar()->addMenu(QStringLiteral("Window"));
  window_menu->addAction(spectrometer_action_);
  window_menu->addAction(experiment_action_);
  data_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
  data_action_->setEnabled(false);
  window_menu->addAction(data_action_);
  connect(data_action_, &QAction::triggered, this, [this] {
    if (data_source_ == nullptr) return;
    if (data_window_ == nullptr) {
      data_window_ = new DataBrowserWindow(*data_source_, this);
      connect(data_window_, &DataBrowserWindow::recall_requested, this, [this](const QString& id) { open_recall(id); });
      connect(data_window_, &DataBrowserWindow::figure_requested, this,
              [this](const QString& kind, const QStringList& ids) { open_figure(kind, ids); });
    }
    data_window_->show();
    data_window_->raise();
    data_window_->activateWindow();
  });
  connect(experiment_action_, &QAction::triggered, this, [this] {
    if (experiment_ == nullptr) {
      return;
    }
    if (experiment_window_ == nullptr) {
      experiment_window_ = new ExperimentWindow(*experiment_, experiment_simulation_,
                                                experiment_settings_ ? experiment_settings_() : nullptr, this);
      experiment_window_->setAttribute(Qt::WA_DeleteOnClose, false);
      if (experiment_queue_) {
        QString error;
        if (!experiment_window_->load_queue(*experiment_queue_, &error)) {
          log_->append_line(QStringLiteral("ERROR [ui] queue not opened: ") + error);
        }
      }
    }
    experiment_window_->show();
    experiment_window_->raise();
    experiment_window_->activateWindow();
  });
  connect(spectrometer_action_, &QAction::triggered, this, [this] {
    if (spectrometer_ == nullptr) {
      return;
    }
    if (spectrometer_window_ == nullptr) {
      spectrometer_window_ = new SpectrometerWindow(*spectrometer_, simulation_,
                                                    settings_factory_ ? settings_factory_() : nullptr, this);
      spectrometer_window_->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    spectrometer_window_->show();
    spectrometer_window_->raise();
    spectrometer_window_->activateWindow();
  });

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

void MainWindow::set_spectrometer(SpectrometerBridge* bridge, bool simulation,
                                  std::function<std::unique_ptr<QSettings>()> settings) {
  if (spectrometer_window_ != nullptr) {
    spectrometer_window_->close();  // saves settings and queues the scan stop
    delete spectrometer_window_;    // it holds the old bridge
    spectrometer_window_ = nullptr;
  }
  spectrometer_ = bridge;
  simulation_ = simulation;
  settings_factory_ = std::move(settings);
  spectrometer_action_->setEnabled(bridge != nullptr);
}

void MainWindow::set_experiment(ExperimentBridge* bridge, bool simulation, std::optional<std::filesystem::path> queue,
                                std::function<std::unique_ptr<QSettings>()> settings) {
  if (experiment_window_ != nullptr) {
    // A hidden window has already been through its close (and any unsaved
    // question); closing it again at teardown would ask after the event loop.
    if (experiment_window_->isVisible()) experiment_window_->close();
    delete experiment_window_;  // it holds the old bridge
    experiment_window_ = nullptr;
  }
  experiment_ = bridge;
  experiment_simulation_ = simulation;
  experiment_queue_ = std::move(queue);
  experiment_settings_ = std::move(settings);
  experiment_action_->setEnabled(bridge != nullptr);
}

MainWindow::~MainWindow() { set_data(nullptr, nullptr); }

void MainWindow::set_data(processing::IAnalysisSource* source, processing::PresetStore* presets) {
  for (auto& w : data_children_)
    if (w) delete w.data();  // they hold the old bridge and source
  data_children_.clear();
  delete data_window_;
  data_window_ = nullptr;
  processing_.reset();
  data_source_ = source;
  presets_ = presets;
  if (source != nullptr && presets != nullptr) processing_ = std::make_unique<ProcessingBridge>(*source);
  data_action_->setEnabled(processing_ != nullptr);
}

QWidget* MainWindow::open_recall(const QString& uuid) {
  if (data_source_ == nullptr) return nullptr;
  auto* w = new RecallWindow(*data_source_, this);
  w->setAttribute(Qt::WA_DeleteOnClose);
  if (!w->show_analysis(uuid)) log_->append_line(QStringLiteral("ERROR [ui] recall failed: ") + uuid);
  data_children_.append(w);
  w->show();
  return w;
}

QWidget* MainWindow::open_time_series(const QStringList& uuids) { return open_figure(QStringLiteral("time_series"), uuids); }

QWidget* MainWindow::open_figure(const QString& kind, const QStringList& uuids) {
  if (!processing_ || presets_ == nullptr) return nullptr;
  if (processing::UnitRegistry::builtin().find(kind.toStdString()) == nullptr) return nullptr;
  if (ReferenceFitWindow::handles(kind.toStdString())) {
    auto* r = new ReferenceFitWindow(*processing_, *presets_, kind.toStdString(), uuids, this);
    r->setAttribute(Qt::WA_DeleteOnClose);
    data_children_.append(r);
    r->show();
    return r;
  }
  auto* w = new FigureWindow(*processing_, *presets_, kind.toStdString(), uuids, this);
  w->setAttribute(Qt::WA_DeleteOnClose);
  connect(w, &FigureWindow::recall_requested, this, [this](const QString& id) { open_recall(id); });
  data_children_.append(w);
  w->show();
  return w;
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (experiment_window_ != nullptr && experiment_window_->isVisible() && !experiment_window_->close()) {
    event->ignore();  // unsaved queue edits, and the user chose Cancel
    return;
  }
  if (spectrometer_window_ != nullptr) {
    spectrometer_window_->close();
  }
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
