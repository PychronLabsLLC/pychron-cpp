#include "main_window.hpp"

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
      data_action_(new QAction(QStringLiteral("Data"), this)),
      data_(new DataWorkspace(this, [this](const QString& text) { log_->append_line(text); })),
      installations_(new QAction(QStringLiteral("Installations…"), this)) {
  setWindowTitle(QStringLiteral("pychron — %1").arg(QString::fromStdString(line.config().system.name)));
  setCentralWidget(canvas_);
  addDockWidget(Qt::BottomDockWidgetArea, log_);
  addDockWidget(Qt::RightDockWidgetArea, alarms_);
  statusBar()->addPermanentWidget(health_, 1);

  spectrometer_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
  spectrometer_action_->setEnabled(false);
  experiment_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
  experiment_action_->setEnabled(false);
  QMenu* file_menu = menuBar()->addMenu(QStringLiteral("File"));
  file_menu->addAction(installations_);
  installations_->setVisible(false);
  connect(installations_, &QAction::triggered, this, [this] {
    if (on_installations_) on_installations_();
  });
  file_menu->addSeparator();
  QAction* quit = file_menu->addAction(QStringLiteral("Quit"));
  quit->setShortcut(QKeySequence::Quit);
  connect(quit, &QAction::triggered, this, &QMainWindow::close);
  QMenu* window_menu = menuBar()->addMenu(QStringLiteral("Window"));
  window_menu->addAction(spectrometer_action_);
  window_menu->addAction(experiment_action_);
  data_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
  data_action_->setEnabled(false);
  window_menu->addAction(data_action_);
  connect(data_action_, &QAction::triggered, this, [this] {
    DataBrowserWindow* w = data_->browser();
    if (w == nullptr) return;
    w->show();
    w->raise();
    w->activateWindow();
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
  data_->set_source(source, presets);
  data_action_->setEnabled(data_->enabled());
}

void MainWindow::set_installations_handler(std::function<void()> handler) {
  on_installations_ = std::move(handler);
  installations_->setVisible(static_cast<bool>(on_installations_));
}

QWidget* MainWindow::open_recall(const QString& uuid) { return data_->open_recall(uuid); }

QWidget* MainWindow::open_time_series(const QStringList& uuids) { return open_figure(QStringLiteral("time_series"), uuids); }

QWidget* MainWindow::open_figure(const QString& kind, const QStringList& uuids) { return data_->open_figure(kind, uuids); }

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
