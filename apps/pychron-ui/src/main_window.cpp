#include "main_window.hpp"

#include "command_palette.hpp"
#include "menu_hub.hpp"
#include "shortcuts.hpp"

#include <utility>

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
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
      installations_(new QAction(QStringLiteral("Installations…"), this)),
      preferences_(new QAction(QStringLiteral("Preferences…"), this)) {
  setWindowTitle(QStringLiteral("pychron — %1").arg(QString::fromStdString(line.config().system.name)));
  setCentralWidget(canvas_);
  addDockWidget(Qt::BottomDockWidgetArea, log_);
  addDockWidget(Qt::RightDockWidgetArea, alarms_);
  statusBar()->addPermanentWidget(health_, 1);

  spectrometer_action_->setShortcut(key(Shortcut::SpectrometerWindow));
  spectrometer_action_->setEnabled(false);
  experiment_action_->setShortcut(key(Shortcut::ExperimentWindow));
  experiment_action_->setEnabled(false);
  installations_->setVisible(false);
  connect(installations_, &QAction::triggered, this, [this] {
    if (on_installations_) on_installations_();
  });
  preferences_->setShortcut(key(Shortcut::Preferences));
  preferences_->setMenuRole(QAction::PreferencesRole);
  // From any window (the bar is the same in all): over the one in front.
  connect(preferences_, &QAction::triggered, this, [this] { open_preferences(preferences_parent()); });
  auto* quit = new QAction(QStringLiteral("Quit"), this);
  quit->setShortcut(key(Shortcut::Quit));
  quit->setMenuRole(QAction::QuitRole);
  connect(quit, &QAction::triggered, this, &QMainWindow::close);
  // Back here from any window: every window shows the same bar.
  auto* line_window = new QAction(QStringLiteral("Extraction Line"), this);
  line_window->setShortcut(key(Shortcut::ExtractionLineWindow));
  connect(line_window, &QAction::triggered, this, [this] {
    showNormal();
    raise();
    activateWindow();
  });
  data_action_->setShortcut(key(Shortcut::DataWindow));
  data_action_->setEnabled(false);
  auto& menus = MenuHub::instance();
  menus.contribute(this, MenuHub::Menu::File, {installations_, preferences_}, MenuHub::Scope::App);
  menus.contribute(this, MenuHub::Menu::File, {quit}, MenuHub::Scope::App);
  menus.contribute(this, MenuHub::Menu::Window, {line_window, spectrometer_action_, experiment_action_, data_action_},
                   MenuHub::Scope::App);
  MenuHub::instance().contribute(this, MenuHub::Menu::Help,
                                 {make_command_palette_action(this), make_shortcuts_action(this)}, MenuHub::Scope::App);
  about_action_ = brand::add_help_menu(this);
  MenuHub::instance().install(this);
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
      spectrometer_window_ = new SpectrometerWindow(*spectrometer_, simulation_, spectrometer_settings(), this);
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

std::unique_ptr<QSettings> MainWindow::spectrometer_settings() const {
  return settings_factory_ ? settings_factory_() : std::make_unique<QSettings>();
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

void MainWindow::set_preferences_settings(PreferencesDialog::SettingsFactory settings) {
  preferences_settings_ = std::move(settings);
}

// The spectrometer's move threshold goes through its window when one is open
// (it keeps the value in use), else straight to its saved settings, which the
// window reads when it opens.
QWidget* MainWindow::preferences_parent() {
  QWidget* front = QApplication::activeWindow();
  if (front == nullptr || qobject_cast<QDialog*>(front) != nullptr) return this;
  return front;
}

PreferencesDialog* MainWindow::open_preferences(QWidget* over) {
  std::optional<double> confirm_move;
  if (spectrometer_window_ != nullptr) {
    confirm_move = spectrometer_window_->confirm_move_amu();
  } else if (spectrometer_ != nullptr) {
    confirm_move = SpectrometerWindow::saved_confirm_move_amu(*spectrometer_settings(), spectrometer_->name());
  }
  return PreferencesDialog::show_for(
      over != nullptr ? over : this, preferences_dialog_, preferences_settings_, confirm_move, [this](const PreferencesDialog::Values& values) {
        apply_preferences(values.preferences);
        if (!values.confirm_move_amu) return;
        if (spectrometer_window_ != nullptr) {
          spectrometer_window_->set_confirm_move_amu(*values.confirm_move_amu);
        } else if (spectrometer_ != nullptr) {
          SpectrometerWindow::save_confirm_move_amu(*spectrometer_settings(), spectrometer_->name(),
                                                    *values.confirm_move_amu);
        }
      });
}

void MainWindow::apply_preferences(const Preferences& preferences) {
  apply_application_preferences(preferences);
  data_->set_page_size(preferences.browser_page_size);
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
