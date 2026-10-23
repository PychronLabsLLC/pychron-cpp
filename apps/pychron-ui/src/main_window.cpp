#include "main_window.hpp"

#include "command_palette.hpp"
#include "menu_hub.hpp"
#include "shortcuts.hpp"
#include "theme.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStatusBar>

#include "experiment_bridge.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/lab/scripts.hpp"

namespace pychron::ui {

namespace {

// The names of the gauges the line defines: the only ones the history keeps.
std::vector<std::string> gauge_names(const config::SystemConfig& config) {
  std::vector<std::string> names;
  names.reserve(config.gauges.size());
  for (const auto& gauge : config.gauges) names.push_back(gauge.name);
  return names;
}

}  // namespace

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

MainWindow::MainWindow(systems::ExtractionLine& line, std::unique_ptr<QSettings> settings, QWidget* parent)
    : QMainWindow(parent),
      settings_(std::move(settings)),
      bridge_(line),
      // Two readings further apart than three scans have a break between them.
      history_(gauge_names(line.config()), std::chrono::milliseconds(3 * line.config().system.scan_interval_ms)),
      canvas_(new CanvasView(bridge_, this)),
      log_(new LogDock(this)),
      // Both tell the time by the line's clock, which stamped what they show.
      alarms_(new AlarmDock(this, [this] { return bridge_.wall_now(); })),
      health_(new HealthBar(this, [this] { return bridge_.now(); })),
      spectrometer_action_(new QAction(QStringLiteral("Spectrometer"), this)),
      field_table_action_(new QAction(QStringLiteral("Field Table…"), this)),
      peak_center_action_(new QAction(QStringLiteral("Peak Center…"), this)),
      experiment_action_(new QAction(QStringLiteral("Experiment"), this)),
      data_action_(new QAction(QStringLiteral("Data"), this)),
      laser_action_(new QAction(QStringLiteral("Laser"), this)),
      data_(new DataWorkspace(this, [this](const QString& text) { log_->append_line(text); })),
      installations_(new QAction(QStringLiteral("Installations…"), this)),
      preferences_(new QAction(QStringLiteral("Preferences…"), this)),
      log_hub_(line.log_hub()) {
  setWindowTitle(QStringLiteral("pychron — %1").arg(QString::fromStdString(line.config().system.name)));
  resize(kDefaultSize);  // the saved geometry, if any, wins below
  setCentralWidget(canvas_);
  log_->set_fed_by(bridge_.bus());
  if (bridge_.cryo_outputs() > 0 || !bridge_.cryo_inputs().empty()) {
    cryo_ = new CryoDock(bridge_, this);
    connect(cryo_, &CryoDock::cryoFailed, log_,
            [this](const QString& message) { log_->append_line(QStringLiteral("ERROR [cryo] ") + message); });
  }
  if (!line.config().heaters.empty()) {
    heaters_ = new HeaterDock(bridge_, this);
    connect(heaters_, &HeaterDock::heaterFailed, log_,
            [this](const QString& message) { log_->append_line(QStringLiteral("ERROR [heater] ") + message); });
  }
  default_layout();
  // Per line: which panels there are depends on it.
  layouts_ = new DockLayouts(this, [this] { default_layout(); }, settings_.get(),
                             QStringLiteral("main_window/%1").arg(QString::fromStdString(line.config().system.name)));
  layouts_->restore_last();
  connect(layouts_, &DockLayouts::applyFailed, log_,
          [this](const QString& message) { log_->append_line(QStringLiteral("WARN [ui] ") + message); });
  statusBar()->addPermanentWidget(health_, 1);

  spectrometer_action_->setShortcut(key(Shortcut::SpectrometerWindow));
  spectrometer_action_->setEnabled(false);
  field_table_action_->setEnabled(false);
  field_table_action_->setToolTip(QStringLiteral("no spectrometer configured"));
  peak_center_action_->setEnabled(false);
  peak_center_action_->setToolTip(QStringLiteral("no spectrometer configured"));
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
  build_file_entries();
  menus.contribute(this, MenuHub::Menu::File, {installations_, preferences_}, MenuHub::Scope::App);
  menus.contribute(this, MenuHub::Menu::File, {quit}, MenuHub::Scope::App);
  menus.contribute(this, MenuHub::Menu::View,
                   {line_window, spectrometer_action_, experiment_action_, data_action_, laser_action_},
                   MenuHub::Scope::App);
  menus.contribute(this, MenuHub::Menu::Spectrometer, {field_table_action_, peak_center_action_},
                   MenuHub::Scope::App);
  // A script stepped against a copy of the line: from every window, greyed
  // until there is a session's lab to pick from.
  interlocks_ = line.switches().interlock_sets();
  rehearse_action_ = new QAction(tr("Rehearse Script…"), this);
  rehearse_action_->setEnabled(false);
  rehearse_action_->setToolTip(tr("Step through a script against a copy of the line; nothing is actuated"));
  connect(rehearse_action_, &QAction::triggered, this, [this] { rehearse_picked(); });
  pick_rehearsal_ = [this](const QStringList& names) -> std::optional<QString> {
    bool ok = false;
    const QString name =
        QInputDialog::getItem(preferences_parent(), tr("Rehearse Script"), tr("Script"), names, 0, false, &ok);
    return ok ? std::optional<QString>(name) : std::nullopt;
  };
  menus.contribute(this, MenuHub::Menu::Scripts, {rehearse_action_}, MenuHub::Scope::App);
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
  connect(experiment_action_, &QAction::triggered, this, [this] { show_experiment_window(); });
  connect(field_table_action_, &QAction::triggered, this, [this] {
    if (spectrometer_ == nullptr || spectrometer_->data_root().empty()) return;
    if (field_table_window_ == nullptr) {
      field_table_window_ =
          new FieldTableEditorWindow(spectrometer_, spectrometer_->data_root(), spectrometer_->config(),
                                     spectrometer_->weights(), [this] { return bridge_.wall_now(); }, this);
      field_table_window_->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    field_table_window_->show();
    field_table_window_->raise();
    field_table_window_->activateWindow();
  });
  connect(peak_center_action_, &QAction::triggered, this, [this] {
    if (spectrometer_ == nullptr) return;
    if (peak_center_window_ == nullptr) {
      peak_center_window_ = new PeakCenterWindow(*spectrometer_, simulation_, spectrometer_settings(), this);
      peak_center_window_->setAttribute(Qt::WA_DeleteOnClose, false);
      peak_center_window_->set_peak_center_file(peak_center_file_);
      peak_center_window_->set_queue_running([this] { return experiment_ != nullptr && experiment_->running(); });
    }
    peak_center_window_->show();
    peak_center_window_->raise();
    peak_center_window_->activateWindow();
  });
  connect(spectrometer_action_, &QAction::triggered, this, [this] {
    if (spectrometer_ == nullptr) {
      return;
    }
    if (spectrometer_window_ == nullptr) {
      spectrometer_window_ = new SpectrometerWindow(*spectrometer_, simulation_, spectrometer_settings(), this);
      spectrometer_window_->setAttribute(Qt::WA_DeleteOnClose, false);
      spectrometer_window_->set_wall_clock([this] { return bridge_.wall_now(); });
      spectrometer_window_->set_rise_rate_file(rise_rate_file_);
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
  // A gauge's history: a rest on its chip shows the last ten minutes, a
  // click opens its window. The popup is the main window's (it places
  // itself beside the chip and hides with the window).
  gauge_popup_ = new GaugePopup(
      history_, [this] { return bridge_.now(); }, [this](TimePoint ts) { return wall_of(ts); }, this);
  connect(&bridge_, &CoreBridge::pressureSample, this, &MainWindow::on_pressure_sample);
  connect(canvas_, &CanvasView::gauge_rested, gauge_popup_, &GaugePopup::show_for);
  connect(canvas_, &CanvasView::gauge_left, gauge_popup_, &QWidget::hide);
  connect(canvas_, &CanvasView::gauge_clicked, this, [this](const QString& name) {
    gauge_popup_->hide();
    open_gauge_window(name);
  });

  connect(&bridge_, &CoreBridge::actuationFinished, log_, [this](const QString& name, const Result<void>& r) {
    if (!r) {
      log_->append_error("line", tr("%1 rejected").arg(name), QString::fromStdString(to_string(r.error())));
    }
  });
}

// The panels as installed: the log along the bottom, the rest down the right.
// Run again by Reset Layout, so it undoes whatever was done to a panel.
void MainWindow::default_layout() {
  const auto place = [this](QDockWidget* dock, Qt::DockWidgetArea area) {
    if (dock == nullptr) return;
    dock->setFloating(false);
    addDockWidget(area, dock);
    dock->show();
  };
  place(log_, Qt::BottomDockWidgetArea);
  place(alarms_, Qt::RightDockWidgetArea);
  place(cryo_, Qt::RightDockWidgetArea);
  place(heaters_, Qt::RightDockWidgetArea);
  // A line with both shares one tab stack: stacked, their charts' minimum
  // heights make the window taller than a laptop screen.
  QDockWidget* tabbed = nullptr;
  if (cryo_ != nullptr && heaters_ != nullptr) {
    tabifyDockWidget(cryo_, heaters_);
    cryo_->raise();
    tabbed = heaters_;
  }
  // And the sizes they ask for, not the ones they were dragged to.
  QList<QDockWidget*> side;
  QList<int> widths;
  QList<int> heights;
  for (QDockWidget* dock : {static_cast<QDockWidget*>(alarms_), static_cast<QDockWidget*>(cryo_),
                            static_cast<QDockWidget*>(heaters_)}) {
    if (dock == tabbed) continue;
    if (dock == nullptr) continue;
    side.append(dock);
    widths.append(dock->sizeHint().width());
    heights.append(dock->sizeHint().height());
  }
  resizeDocks(side, widths, Qt::Horizontal);
  resizeDocks(side, heights, Qt::Vertical);
  resizeDocks({log_}, {log_->sizeHint().height()}, Qt::Vertical);
}

std::unique_ptr<QSettings> MainWindow::spectrometer_settings() const {
  return settings_factory_ ? settings_factory_() : std::make_unique<QSettings>();
}

void MainWindow::set_spectrometer(SpectrometerBridge* bridge, bool simulation,
                                  std::function<std::unique_ptr<QSettings>()> settings) {
  if (peak_center_window_ != nullptr) {
    peak_center_window_->cancel_peak_center_and_wait();  // a run cannot outlive its window; no question
    peak_center_window_->close();                        // saves its splitter and geometry
    delete peak_center_window_;  // it holds the old bridge
    peak_center_window_ = nullptr;
  }
  if (spectrometer_window_ != nullptr) {
    spectrometer_window_->close();  // saves settings and queues the scan stop
    delete spectrometer_window_;    // it holds the old bridge
    spectrometer_window_ = nullptr;
  }
  if (field_table_window_ != nullptr) {
    // It holds the old bridge. Unsaved edits were asked about when it was
    // closed; this is the end of the session.
    delete field_table_window_;
    field_table_window_ = nullptr;
  }
  spectrometer_ = bridge;
  simulation_ = simulation;
  settings_factory_ = std::move(settings);
  spectrometer_action_->setEnabled(bridge != nullptr);
  const bool tables = bridge != nullptr && !bridge->data_root().empty();
  field_table_action_->setEnabled(tables);
  field_table_action_->setToolTip(tables ? QStringLiteral("Edit the spectrometer's field tables")
                                         : QStringLiteral("no spectrometer configured"));
  peak_center_action_->setEnabled(bridge != nullptr);
  peak_center_action_->setToolTip(bridge != nullptr ? QStringLiteral("Center a peak and apply it to the field table")
                                                    : QStringLiteral("no spectrometer configured"));
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
  delete rehearsal_window_;  // it holds the old session's lab
  rehearsal_window_ = nullptr;
  for (const auto& connection : experiment_refreshes_) disconnect(connection);  // the old bridge may be gone
  experiment_ = bridge;
  if (bridge != nullptr) {
    // What a run saved is in the data window when the run ends; a queue's
    // start picks up whatever was entered since. The bridge delivers on this
    // thread; queued, so the experiment window hears of the run first.
    experiment_refreshes_[0] =
        connect(bridge, &ExperimentBridge::runFinished, this, &MainWindow::refresh_data, Qt::QueuedConnection);
    experiment_refreshes_[1] =
        connect(bridge, &ExperimentBridge::queueStarted, this, &MainWindow::refresh_data, Qt::QueuedConnection);
    // A held queue is said in the health bar, with the time left before the
    // hold limit ends it, until the executor moves on or the queue ends.
    namespace exec = experiment::executor;
    experiment_refreshes_[2] = connect(bridge, &ExperimentBridge::executorHolding, health_,
                                       [this](const exec::ExecutorHolding& e) {
                                         health_->show_hold(QString::fromStdString(e.reason), e.until);
                                       });
    experiment_refreshes_[3] = connect(bridge, &ExperimentBridge::executorStateChanged, health_,
                                       [this](const exec::ExecutorStateChanged& e) {
                                         if (e.to != exec::ExecutorState::Holding) health_->clear_hold();
                                       });
    experiment_refreshes_[4] = connect(bridge, &ExperimentBridge::queueEnded, health_,
                                       [this](const experiment::lab::QueueEnded&) { health_->clear_hold(); });
    // The Peak Center window follows the queue: Start and Apply are off
    // while it runs, and a result seen then is stale.
    experiment_refreshes_[5] = connect(bridge, &ExperimentBridge::queueStarted, this, [this] {
      if (peak_center_window_ != nullptr) peak_center_window_->queue_state_changed();
    });
    experiment_refreshes_[6] = connect(bridge, &ExperimentBridge::queueEnded, this, [this] {
      if (peak_center_window_ != nullptr) peak_center_window_->queue_state_changed();
    });
  }
  health_->clear_hold();  // a hold of the old bridge's queue
  experiment_simulation_ = simulation;
  experiment_queue_ = std::move(queue);
  experiment_settings_ = std::move(settings);
  experiment_action_->setEnabled(bridge != nullptr);
  for (QAction* entry : file_entries_) entry->setEnabled(bridge != nullptr);
  rehearse_action_->setEnabled(bridge != nullptr);
}

// The experiment window, made on first use; nullptr without a session.
ExperimentWindow* MainWindow::ensure_experiment_window() {
  if (experiment_ == nullptr) return nullptr;
  if (experiment_window_ == nullptr) {
    experiment_window_ = new ExperimentWindow(*experiment_, experiment_simulation_,
                                              experiment_settings_ ? experiment_settings_() : nullptr, this);
    experiment_window_->setAttribute(Qt::WA_DeleteOnClose, false);
    experiment_window_->factory()->set_identifier_source(identifier_source_);
    experiment_window_->set_peak_center_running(
        [this] { return peak_center_window_ != nullptr && peak_center_window_->peak_center_running(); });
    give_references();
    connect(experiment_window_, &ExperimentWindow::rehearseRequested, this,
            [this](scripting::ScriptKind kind, const QString& name, const QString& text) { rehearse(kind, name, text); });
    if (experiment_queue_) {
      QString error;
      // A name given at start is looked for in the lab's queues/, then the
      // lab (queue-directory design, section 3).
      const auto queue = experiment::lab::find_queue(*experiment_queue_, experiment_->lab().paths.dir);
      if (!experiment_window_->load_queue(queue, &error)) {
        log_->append_line(QStringLiteral("ERROR [ui] queue not opened: ") + error);
      } else {
        experiment_window_->offer_resume();
      }
    }
  }
  return experiment_window_;
}

RehearsalWindow* MainWindow::rehearse(scripting::ScriptKind kind, const QString& name, const QString& text) {
  if (experiment_ == nullptr) return nullptr;
  if (rehearsal_window_ == nullptr) {
    if (!rehearsal_host_) rehearsal_host_ = scripting::make_script_host();
    rehearsal_window_ = new RehearsalWindow(*rehearsal_host_, bridge_, interlocks_, experiment_->lab(),
                                            experiment_settings_ ? experiment_settings_() : nullptr, this);
    rehearsal_window_->setWindowFlag(Qt::Window, true);
    rehearsal_window_->setAttribute(Qt::WA_DeleteOnClose, false);
  }
  // Run fields from make_context(): a queue row naming the script is not
  // looked for (spec 6.2's queue defaults are not wired yet).
  rehearsal_window_->set_script(kind, name, text);
  rehearsal_window_->show();
  rehearsal_window_->raise();
  rehearsal_window_->activateWindow();
  return rehearsal_window_;
}

bool MainWindow::rehearse_picked() {
  if (experiment_ == nullptr) return false;
  std::vector<experiment::lab::ScriptFile> files;
  QStringList names;
  for (auto& f : experiment::lab::lab_scripts(experiment_->lab())) {
    // Hooks are refused by rehearse(); lib/ holds gosub targets, not scripts a run starts.
    if (f.kind == scripting::ScriptKind::MeasurementHook || f.name.starts_with("lib:")) continue;
    names << QString::fromStdString(std::string(scripting::to_string(f.kind)) + "/" + f.name);
    files.push_back(std::move(f));
  }
  if (names.isEmpty()) return false;
  const std::optional<QString> picked = pick_rehearsal_(names);
  if (!picked) return false;
  const auto at = names.indexOf(*picked);
  if (at < 0) return false;
  const auto& file = files[static_cast<std::size_t>(at)];
  std::ifstream in(file.path, std::ios::binary);
  if (!in) return false;
  std::stringstream text;
  text << in.rdbuf();
  return rehearse(file.kind, QString::fromStdString(file.name), QString::fromStdString(text.str())) != nullptr;
}

void MainWindow::set_identifier_source(IdentifierSource* source) {
  identifier_source_ = source;
  if (experiment_window_ != nullptr) experiment_window_->factory()->set_identifier_source(source);
}

ExperimentWindow* MainWindow::show_experiment_window() {
  ExperimentWindow* window = ensure_experiment_window();
  if (window == nullptr) return nullptr;
  window->show();
  window->raise();
  window->activateWindow();
  return window;
}

// File > New and File > Open: what there is to start or open, from any
// window. Each brings up the window it acts in (the experiment window, or
// one of its editors alone) and then asks what that window asks. The key of
// New and of Open goes to the entry whose window is in front (MenuHub).
void MainWindow::build_file_entries() {
  using List = MenuHub::FileList;
  const auto entry = [this](const QString& text, std::function<void()> run,
                            std::function<bool(const QWidget*)> home) {
    auto* action = new QAction(text, this);
    action->setEnabled(false);  // until there is a session (set_experiment)
    connect(action, &QAction::triggered, this, [run = std::move(run)] { run(); });
    file_entries_.append(action);
    return MenuHub::FileEntry{action, std::move(home)};
  };
  const auto in_experiment = [this](const QWidget* w) { return experiment_window_ != nullptr && w == experiment_window_; };
  const auto in_scripts = [this](const QWidget* w) {
    return experiment_window_ != nullptr && experiment_window_->script_editor() != nullptr &&
           w == experiment_window_->script_editor();
  };
  const auto in_conditionals = [this](const QWidget* w) {
    return experiment_window_ != nullptr && experiment_window_->conditionals_editor() != nullptr &&
           w == experiment_window_->conditionals_editor();
  };
  auto& menus = MenuHub::instance();
  menus.contribute_file(
      this, List::New,
      {entry(tr("New &Queue"), [this] { if (auto* w = show_experiment_window()) w->new_dialog(); }, in_experiment),
       entry(tr("New &Script…"),
             [this] { if (auto* w = ensure_experiment_window()) w->open_script_editor()->new_dialog(); }, in_scripts),
       entry(tr("New &Conditionals…"),
             [this] { if (auto* w = ensure_experiment_window()) w->open_conditionals_editor()->new_dialog(); },
             in_conditionals)});
  menus.contribute_file(
      this, List::Open,
      {entry(tr("Open &Queue…"), [this] { if (auto* w = show_experiment_window()) w->open_dialog(); }, in_experiment),
       entry(tr("Open &Script…"),
             [this] { if (auto* w = ensure_experiment_window()) w->open_script_editor()->open_picked(); }, in_scripts),
       entry(tr("Open &Conditionals…"),
             [this] { if (auto* w = ensure_experiment_window()) w->open_conditionals_editor()->open_picked(); },
             in_conditionals)});
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
          log_->append_error("laser", QStringLiteral("%1 %2").arg(device, what), QString::fromStdString(r.error().what));
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

// Readings go into the history, then to whatever shows that gauge. The
// values a start's snapshot carries are not here: they have no time.
void MainWindow::on_pressure_sample(const PressureSample& sample) {
  history_.add(sample.gauge, sample.value, sample.units, sample.ts);
  const QString name = QString::fromStdString(sample.gauge);
  if (gauge_popup_->isVisible() && gauge_popup_->gauge() == name) gauge_popup_->refresh();
  if (GaugeWindow* window = gauge_window(name)) window->refresh();
}

QDateTime MainWindow::wall_of(TimePoint ts) const {
  const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(bridge_.now() - ts);
  return bridge_.wall_now().addMSecs(-age.count());
}

void MainWindow::set_gauge_settings(std::function<std::unique_ptr<QSettings>()> settings) {
  gauge_settings_ = std::move(settings);
}

GaugeWindow* MainWindow::gauge_window(const QString& name) const {
  const auto it = gauge_windows_.find(name);
  return it == gauge_windows_.end() ? nullptr : it->second;
}

GaugeWindow* MainWindow::open_gauge_window(const QString& name) {
  GaugeWindow* window = gauge_window(name);
  if (window == nullptr) {
    const auto& gauges = bridge_.config().gauges;
    const auto gauge = std::find_if(gauges.begin(), gauges.end(),
                                    [&name](const config::GaugeConfig& g) { return QString::fromStdString(g.name) == name; });
    if (gauge == gauges.end()) return nullptr;
    GaugeWindow::Deps deps;
    deps.history = &history_;
    deps.wall_of = [this](TimePoint ts) { return wall_of(ts); };
    deps.alarm_high = gauge->alarm_high;
    deps.alarm_low = gauge->alarm_low;
    deps.settings = [this] { return gauge_settings_ ? gauge_settings_() : std::make_unique<QSettings>(); };
    deps.line = QString::fromStdString(bridge_.config().system.name);
    // A window of its own (GaugeWindow is a Qt::Window), hidden on close and kept.
    window = new GaugeWindow(name, std::move(deps), this);
    gauge_windows_.emplace(name, window);
  }
  window->show();
  window->raise();
  window->activateWindow();
  return window;
}

MainWindow::~MainWindow() {
  // It reads the bridge and the host, which are members.
  delete rehearsal_window_;
  rehearsal_window_ = nullptr;
  // Qt would delete these children after the history they read is gone.
  delete gauge_popup_;
  gauge_popup_ = nullptr;
  for (auto& [name, window] : gauge_windows_) delete window;
  gauge_windows_.clear();
  set_lasers({}, false);
  set_data(nullptr, nullptr);
}

void MainWindow::set_data(processing::IAnalysisSource* source, processing::PresetStore* presets, bool session_store) {
  // The References dock reads through the workspace's bridge, which
  // set_source destroys: the dock goes first.
  if (experiment_window_ != nullptr) experiment_window_->set_references(nullptr, nullptr);
  data_->set_source(source, presets);
  presets_ = presets;
  session_store_ = session_store && source != nullptr && presets != nullptr;
  data_action_->setEnabled(data_->enabled());
  give_references();
}

void MainWindow::give_references() {
  if (experiment_window_ != nullptr && session_store_) experiment_window_->set_references(processing_bridge(), presets_);
}

void MainWindow::set_data_unavailable(const QString& reason) { data_->set_unavailable(reason); }

void MainWindow::refresh_data() {
  if (DataBrowserWindow* browser = data_->existing_browser()) browser->refresh_if_changed();
}

void MainWindow::set_installations_handler(std::function<void()> handler) {
  on_installations_ = std::move(handler);
  installations_->setVisible(static_cast<bool>(on_installations_));
}

void MainWindow::set_preferences_settings(PreferencesDialog::SettingsFactory settings) {
  preferences_settings_ = std::move(settings);
  data_->set_settings(preferences_settings_);
}

void MainWindow::set_line_config_file(std::filesystem::path main_file) { line_config_file_ = std::move(main_file); }

void MainWindow::set_install_root(std::filesystem::path root) { install_root_ = std::move(root); }

void MainWindow::set_metrics_status(QString status) { metrics_status_ = std::move(status); }

// The spectrometer's move threshold goes through its window when one is open
// (it keeps the value in use), else straight to its saved settings, which the
// window reads when it opens.
QWidget* MainWindow::preferences_parent() {
  QWidget* front = QApplication::activeWindow();
  if (front == nullptr || qobject_cast<QDialog*>(front) != nullptr) return this;
  return front;
}

void MainWindow::set_data_source(DataSourceDepsFactory deps, PreferencesDialog::Reopen reopen) {
  data_source_deps_ = std::move(deps);
  data_source_reopen_ = std::move(reopen);
}

PreferencesDialog* MainWindow::open_preferences(QWidget* over) {
  std::optional<double> confirm_move;
  if (spectrometer_window_ != nullptr) {
    confirm_move = spectrometer_window_->confirm_move_amu();
  } else if (spectrometer_ != nullptr) {
    confirm_move = SpectrometerWindow::saved_confirm_move_amu(*spectrometer_settings(), spectrometer_->name());
  }
  // The line's logging and metrics are read from its files each time: what
  // was applied a moment ago is in them, not in the config the line loaded.
  std::optional<LineSettings> line;
  if (!line_config_file_.empty()) {
    line = load_line_settings(line_config_file_, install_root_);
    if (line) line->metrics_status = metrics_status_;
  }
  PreferencesDialog* dialog = PreferencesDialog::show_for(
      over != nullptr ? over : this, preferences_dialog_, preferences_settings_, confirm_move,
      [this](const PreferencesDialog::Values& values) {
        apply_preferences(values.preferences);
        if (!values.confirm_move_amu) return;
        if (spectrometer_window_ != nullptr) {
          spectrometer_window_->set_confirm_move_amu(*values.confirm_move_amu);
        } else if (spectrometer_ != nullptr) {
          SpectrometerWindow::save_confirm_move_amu(*spectrometer_settings(), spectrometer_->name(),
                                                    *values.confirm_move_amu);
        }
      },
      std::move(line),
      [this](const LineSettings& wanted) {
        const std::optional<LineSettings> before = load_line_settings(line_config_file_, install_root_);
        const QString failed = save_line_settings(line_config_file_, wanted);
        if (!failed.isEmpty()) return failed;
        // Saved. Where the log file goes and the metrics endpoint are read
        // at the next start; the levels and the Log panel's level are in
        // force from now, when they are what changed. An Apply about
        // something else leaves the hub alone, and with it a level set for
        // the session from the log panel.
        const auto levels = [](const config::LoggingConfig& l) {
          return std::map<std::string, LogLevel>(l.levels.begin(), l.levels.end());
        };
        const bool changed = !before || before->logging.default_level != wanted.logging.default_level ||
                             levels(before->logging) != levels(wanted.logging);
        const bool dock_changed = !before || before->logging.dock_level != wanted.logging.dock_level;
        if (const auto hub = log_hub_.lock()) {
          if (changed) hub->set_levels(wanted.logging.default_level, wanted.logging.levels);
          if (dock_changed) hub->set_dock_level(wanted.logging.dock_level);
        }
        return QString();
      });
  // The data source is not changed under a running queue.
  // Read for a new dialog only: one raised again keeps its page, which holds
  // the install as its last apply left it.
  if (data_source_deps_ && dialog->data_source_page() == nullptr) {
    if (auto deps = data_source_deps_()) {
      if (!deps->queue_running) deps->queue_running = [this] { return experiment_ != nullptr && experiment_->running(); };
      dialog->add_data_source_page(std::move(*deps), data_source_reopen_);
    }
  }
  return dialog;
}

void MainWindow::apply_preferences(const Preferences& preferences) {
  apply_application_preferences(preferences);
  data_->apply_preferences(preferences);
}

QWidget* MainWindow::open_recall(const QString& uuid) { return data_->open_recall(uuid); }

QWidget* MainWindow::open_time_series(const QStringList& uuids) { return open_figure(QStringLiteral("time_series"), uuids); }

QWidget* MainWindow::open_figure(const QString& kind, const QStringList& uuids) { return data_->open_figure(kind, uuids); }

void MainWindow::closeEvent(QCloseEvent* event) {
  if (experiment_window_ != nullptr && experiment_window_->isVisible() && !experiment_window_->close()) {
    event->ignore();  // unsaved queue edits, and the user chose Cancel
    return;
  }
  if (field_table_window_ != nullptr && field_table_window_->isVisible() && !field_table_window_->close()) {
    event->ignore();  // unsaved field table edits, and the user chose Cancel
    return;
  }
  if (peak_center_window_ != nullptr) {
    peak_center_window_->cancel_peak_center_and_wait();  // quitting: no question
    peak_center_window_->close();
  }
  if (spectrometer_window_ != nullptr) spectrometer_window_->close();
  for (auto& [device, window] : laser_windows_) window->close();
  for (auto& [name, window] : gauge_windows_) window->close();
  gauge_popup_->hide();
  if (pattern_maker_ != nullptr) pattern_maker_->close();
  layouts_->save_last();
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
