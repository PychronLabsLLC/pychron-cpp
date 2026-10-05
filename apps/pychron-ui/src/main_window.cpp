#include "main_window.hpp"

#include "command_palette.hpp"
#include "menu_hub.hpp"
#include "shortcuts.hpp"
#include "theme.hpp"

#include <utility>

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QIcon>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStatusBar>

namespace pychron::ui {

// ---- View menu glyphs ---------------------------------------------------------

QIcon MainWindow::view_icon(View view) {
  // Line glyphs on an 18 pt square, drawn at 2x. A mask (template) icon: the
  // platform colours it to suit the menu, light or dark; where it does not,
  // it is drawn in the theme's text colour.
  constexpr int kPoints = 18;
  constexpr int kScale = 2;
  QPixmap pixmap(kPoints * kScale, kPoints * kScale);
  pixmap.setDevicePixelRatio(kScale);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor ink = theme().text;
  p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);
  switch (view) {
    case View::ExtractionLine:
      // a valve in a run of pipe, a volume hanging off it
      p.drawLine(QPointF(1, 6), QPointF(5, 6));
      p.drawRoundedRect(QRectF(5, 3, 6, 6), 1.5, 1.5);
      p.drawLine(QPointF(11, 6), QPointF(17, 6));
      p.drawLine(QPointF(14, 6), QPointF(14, 10));
      p.drawRoundedRect(QRectF(10.5, 10, 7, 6), 1.5, 1.5);
      break;
    case View::Spectrometer: {
      // The canvas's spectrometer glyph, on its 66 x 62 grid, as a
      // silhouette: source, the hollow flight tube turning a quarter circle
      // through the magnet's pole piece, and the slitted collector block.
      p.save();
      p.scale(kPoints / 66.0, kPoints / 66.0);
      p.translate(0, 2);
      const QPointF center(46, 48);
      auto ring = [&](double r) { return QRectF(center.x() - r, center.y() - r, 2 * r, 2 * r); };
      QPainterPath tube(QPointF(12, 56));
      tube.lineTo(12, 48);
      tube.arcTo(ring(34), 180, -90);
      tube.lineTo(52, 14);
      p.setPen(QPen(ink, 11, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
      p.drawPath(tube);
      p.setCompositionMode(QPainter::CompositionMode_Clear);  // hollow it out
      p.setPen(QPen(ink, 4, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
      p.drawPath(tube);
      p.setCompositionMode(QPainter::CompositionMode_SourceOver);

      // The magnet and the collector in outline, so each part reads apart
      // from the tube at menu size; the tube is hidden where it passes
      // between the poles.
      QPainterPath magnet;
      magnet.arcMoveTo(ring(46), 162);
      magnet.arcTo(ring(46), 162, -54);
      magnet.arcTo(ring(22), 108, 54);
      magnet.closeSubpath();
      const QPen outline(ink, 3.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      p.setCompositionMode(QPainter::CompositionMode_Clear);
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawPath(magnet);
      p.setCompositionMode(QPainter::CompositionMode_SourceOver);
      p.setBrush(Qt::NoBrush);
      p.setPen(outline);
      p.drawPath(magnet);

      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawRoundedRect(QRectF(2, 50, 20, 12), 2, 2);  // source

      const QRectF collector(51, 1.5, 13, 25);
      p.setCompositionMode(QPainter::CompositionMode_Clear);
      p.drawRect(collector);
      p.setCompositionMode(QPainter::CompositionMode_SourceOver);
      p.setBrush(Qt::NoBrush);
      p.setPen(outline);
      p.drawRoundedRect(collector, 2, 2);
      p.setPen(QPen(ink, 3.2, Qt::SolidLine, Qt::FlatCap));
      for (const double y : {10.0, 18.0}) p.drawLine(QPointF(collector.left() + 3.5, y), QPointF(collector.right() - 3.5, y));
      p.restore();
      break;
    }
    case View::Experiment: {
      // a queue of runs, the first one going
      QPainterPath play(QPointF(2, 2.5));
      play.lineTo(6, 4.75);
      play.lineTo(2, 7);
      play.closeSubpath();
      p.setBrush(ink);
      p.drawPath(play);
      p.setBrush(Qt::NoBrush);
      p.drawLine(QPointF(8.5, 4.75), QPointF(16, 4.75));
      for (const double y : {10.0, 14.5}) {
        p.drawLine(QPointF(2.5, y), QPointF(5, y));
        p.drawLine(QPointF(8.5, y), QPointF(16, y));
      }
      break;
    }
    case View::Laser: {
      // a beam coming down onto a sample, which glows
      p.drawLine(QPointF(4, 2), QPointF(9, 11));
      p.setBrush(ink);
      p.drawEllipse(QPointF(9.6, 12.2), 1.8, 1.8);
      p.setBrush(Qt::NoBrush);
      for (const auto& [from, to] : {std::pair{QPointF(13, 9.5), QPointF(15.5, 8)}, std::pair{QPointF(13.6, 12.5), QPointF(16.5, 12.5)},
                                     std::pair{QPointF(13, 15.2), QPointF(15.5, 16.6)}}) {
        p.drawLine(from, to);
      }
      p.drawLine(QPointF(3, 16.5), QPointF(11.5, 16.5));
      break;
    }
    case View::Data: {
      // axes and the points of a signal decaying toward its intercept
      p.drawLine(QPointF(2.5, 2), QPointF(2.5, 15.5));
      p.drawLine(QPointF(2.5, 15.5), QPointF(16.5, 15.5));
      p.setBrush(ink);
      p.setPen(Qt::NoPen);
      for (const QPointF& point : {QPointF(5.5, 4.5), QPointF(8.5, 8.5), QPointF(12, 10.8), QPointF(15.5, 11.8)}) {
        p.drawEllipse(point, 1.3, 1.3);
      }
      break;
    }
  }
  p.end();
  QIcon icon(pixmap);
  icon.setIsMask(true);
  return icon;
}

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
      laser_action_(new QAction(QStringLiteral("Laser"), this)),
      data_(new DataWorkspace(this, [this](const QString& text) { log_->append_line(text); })),
      installations_(new QAction(QStringLiteral("Installations…"), this)),
      preferences_(new QAction(QStringLiteral("Preferences…"), this)) {
  setWindowTitle(QStringLiteral("pychron — %1").arg(QString::fromStdString(line.config().system.name)));
  setCentralWidget(canvas_);
  addDockWidget(Qt::BottomDockWidgetArea, log_);
  addDockWidget(Qt::RightDockWidgetArea, alarms_);
  if (bridge_.cryo_outputs() > 0 || !bridge_.cryo_inputs().empty()) {
    cryo_ = new CryoDock(bridge_, this);
    addDockWidget(Qt::RightDockWidgetArea, cryo_);
    connect(cryo_, &CryoDock::cryoFailed, log_,
            [this](const QString& message) { log_->append_line(QStringLiteral("ERROR [cryo] ") + message); });
  }
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
  laser_action_->setShortcut(key(Shortcut::LaserWindow));
  laser_action_->setEnabled(false);
  // A glyph each (macOS hides menu icons unless an action asks for its own).
  for (const auto& [action, view] : {std::pair{line_window, View::ExtractionLine},
                                     std::pair{spectrometer_action_, View::Spectrometer},
                                     std::pair{experiment_action_, View::Experiment},
                                     std::pair{data_action_, View::Data},
                                     std::pair{laser_action_, View::Laser}}) {
    action->setIcon(view_icon(view));
    action->setIconVisibleInMenu(true);
  }
  auto& menus = MenuHub::instance();
  menus.contribute(this, MenuHub::Menu::File, {installations_, preferences_}, MenuHub::Scope::App);
  menus.contribute(this, MenuHub::Menu::File, {quit}, MenuHub::Scope::App);
  menus.contribute(this, MenuHub::Menu::View,
                   {line_window, spectrometer_action_, experiment_action_, data_action_, laser_action_},
                   MenuHub::Scope::App);
  MenuHub::instance().contribute(this, MenuHub::Menu::Help,
                                 {make_command_palette_action(this), make_shortcuts_action(this)}, MenuHub::Scope::App);
  about_action_ = brand::add_help_menu(this);
  MenuHub::instance().install(this);
  // The first laser (the only one, usually); with several the action carries
  // a submenu of them.
  connect(laser_action_, &QAction::triggered, this, [this] {
    if (!lasers_.empty()) open_laser(lasers_.front()->device());
  });
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

void MainWindow::set_lasers(std::vector<LaserBridge*> bridges, bool simulation, laser::PatternLibrary* patterns,
                            std::filesystem::path patterns_dir) {
  for (auto& [device, window] : laser_windows_) {
    window->close();
    delete window;  // it holds the old bridge
  }
  laser_windows_.clear();
  delete pattern_maker_;  // it holds the old library
  pattern_maker_ = nullptr;
  lasers_ = std::move(bridges);
  laser_simulation_ = simulation;
  patterns_ = lasers_.empty() ? nullptr : patterns;
  patterns_dir_ = std::move(patterns_dir);
  laser_action_->setEnabled(!lasers_.empty());
  delete laser_action_->menu();
  laser_action_->setMenu(nullptr);
  if (lasers_.size() > 1) {
    auto* menu = new QMenu(this);
    for (LaserBridge* bridge : lasers_) {
      const QString device = bridge->device();
      menu->addAction(device, this, [this, device] { open_laser(device); });
    }
    laser_action_->setMenu(menu);
  }
}

LaserWindow* MainWindow::laser_window(const QString& device) const {
  const auto it = laser_windows_.find(device);
  return it == laser_windows_.end() ? nullptr : it->second;
}

LaserWindow* MainWindow::open_laser(const QString& device) {
  LaserWindow* window = laser_window(device);
  if (window == nullptr) {
    for (LaserBridge* bridge : lasers_) {
      if (bridge->device() != device) continue;
      window = new LaserWindow(*bridge, laser_simulation_, this);
      window->setWindowFlag(Qt::Window, true);
      window->setAttribute(Qt::WA_DeleteOnClose, false);
      window->resize(1000, 720);
      if (patterns_ != nullptr) window->set_pattern_maker([this] { open_pattern_maker(); });
      // a queue's errors are in the log; so are a laser's
      connect(bridge, &LaserBridge::commandFinished, window, [this, device](const QString& what, const Result<void>& r) {
        if (!r && r.error().kind != ErrorKind::Cancelled) {
          log_->append_line(QStringLiteral("ERROR [laser %1] %2: %3").arg(device, what, QString::fromStdString(r.error().what)));
        }
      });
      laser_windows_.emplace(device, window);
    }
  }
  if (window == nullptr) return nullptr;
  window->show();
  window->raise();
  window->activateWindow();
  return window;
}

PatternMakerWindow* MainWindow::open_pattern_maker() {
  if (patterns_ == nullptr) return nullptr;
  if (pattern_maker_ == nullptr) {
    pattern_maker_ = new PatternMakerWindow(*patterns_, patterns_dir_, this);
    pattern_maker_->setWindowFlag(Qt::Window, true);
    pattern_maker_->setAttribute(Qt::WA_DeleteOnClose, false);
    connect(pattern_maker_, &PatternMakerWindow::saved, this, [this](const QString&) {
      for (auto& [device, window] : laser_windows_) window->refresh_patterns();
    });
  }
  pattern_maker_->show();
  pattern_maker_->raise();
  pattern_maker_->activateWindow();
  return pattern_maker_;
}

MainWindow::~MainWindow() {
  set_lasers({}, false);
  set_data(nullptr, nullptr);
}

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
  for (auto& [device, window] : laser_windows_) window->close();
  if (pattern_maker_ != nullptr) pattern_maker_->close();
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
