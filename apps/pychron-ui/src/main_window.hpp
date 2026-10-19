#pragma once

// M1 status/control window (spec section 10.3): canvas in the center, log and
// alarm docks (and the cryostat's and the heaters', when the line has them),
// per-transport health chips in the status bar. Owns the CoreBridge; the
// ExtractionLine must outlive the window. Where the docks are is kept per
// line (dock_layouts.hpp). Keeps the history of the line's gauge readings
// from the moment it exists: resting the pointer on a gauge of the canvas
// shows its last ten minutes, clicking it opens that gauge's window.

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <optional>

#include <QAction>
#include <QString>
#include <QList>
#include <QIcon>
#include <QMainWindow>
#include <QPointer>
#include <QSettings>
#include <QSize>

#include "alarm_dock.hpp"
#include "brand.hpp"
#include "data_browser_window.hpp"
#include "data_workspace.hpp"
#include "dock_layouts.hpp"
#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "cryo_dock.hpp"
#include "heater_dock.hpp"
#include "experiment_window.hpp"
#include "gauge_popup.hpp"
#include "gauge_window.hpp"
#include "health_bar.hpp"
#include "identifier_source.hpp"
#include "laser_bridge.hpp"
#include "laser_window.hpp"
#include "pattern_maker_window.hpp"
#include "log_dock.hpp"
#include "preferences_dialog.hpp"
#include "pychron/core/log_hub.hpp"
#include "processing_bridge.hpp"
#include "reading_history.hpp"
#include "field_table_editor_window.hpp"
#include "spectrometer_window.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::ui {

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `settings` keeps the panel layout between sessions, per line, and the
  // layouts saved under a name; with none (the default) nothing is kept.
  // The size the window has until it is given another (and kept, with settings).
  static constexpr QSize kDefaultSize{1200, 850};

  explicit MainWindow(systems::ExtractionLine& line, std::unique_ptr<QSettings> settings = nullptr,
                      QWidget* parent = nullptr);

  // The glyph beside each View menu item: a line drawing, a mask icon the
  // platform colours to suit its menus.
  enum class View { ExtractionLine, Spectrometer, Experiment, Data, Laser };
  static QIcon view_icon(View view);
  // Closes the data windows before the processing bridge they use goes.
  ~MainWindow() override;

  CoreBridge& bridge() noexcept { return bridge_; }
  CanvasView* canvas_view() const noexcept { return canvas_; }
  LogDock* log_dock() const noexcept { return log_; }
  AlarmDock* alarm_dock() const noexcept { return alarms_; }
  // Null unless the line has a [cryo] cryostat.
  CryoDock* cryo_dock() const noexcept { return cryo_; }
  // Null unless the line has [[heaters]].
  HeaterDock* heater_dock() const noexcept { return heaters_; }
  HealthBar* health_bar() const noexcept { return health_; }
  // Where the panels are: the factory layout, the last one, the named ones.
  DockLayouts* dock_layouts() const noexcept { return layouts_; }

  // Enables View > Spectrometer for `bridge` (null disables it and closes
  // the window). The bridge must outlive the main window or be cleared first.
  // `settings` makes the window's QSettings (default: the application's);
  // tests pass a temp file.
  void set_spectrometer(SpectrometerBridge* bridge, bool simulation,
                        std::function<std::unique_ptr<QSettings>()> settings = {});
  QAction* spectrometer_action() const noexcept { return spectrometer_action_; }
  // Where the spectrometer window appends its rise rate results (empty:
  // nowhere). Set before the window is first opened.
  void set_rise_rate_file(std::filesystem::path path) { rise_rate_file_ = std::move(path); }
  // Null until the action is first triggered.
  SpectrometerWindow* spectrometer_window() const noexcept { return spectrometer_window_; }
  // Spectrometer > Field Table…, in every window's bar: opens the one field
  // table editor (or raises it) on the spectrometer's data root. Disabled,
  // with the reason as tooltip, without a spectrometer or a data root.
  QAction* field_table_action() const noexcept { return field_table_action_; }
  // Null until the action is first triggered, and again after
  // set_spectrometer replaces the bridge.
  FieldTableEditorWindow* field_table_window() const noexcept { return field_table_window_; }

  // Enables View > Experiment for `bridge` (null disables it and closes the
  // window). `queue` is opened the first time the window shows. Same
  // lifetime and settings rules as set_spectrometer.
  void set_experiment(ExperimentBridge* bridge, bool simulation, std::optional<std::filesystem::path> queue = {},
                      std::function<std::unique_ptr<QSettings>()> settings = {});
  QAction* experiment_action() const noexcept { return experiment_action_; }
  ExperimentWindow* experiment_window() const noexcept { return experiment_window_; }
  // Where the experiment window's run factory picks identifiers from (null:
  // typed only). Not owned; given to the window when there is one.
  void set_identifier_source(IdentifierSource* source);
  // File > New and File > Open, in menu order (New Queue, New Script…, New
  // Conditionals…, Open Queue…, Open Script…, Open Conditionals…).
  const QList<QAction*>& file_entries() const noexcept { return file_entries_; }

  // Enables View > Laser for `bridges`, one laser window each (a submenu when
  // there are several); none disables it and closes the laser windows and
  // the pattern maker. With `patterns` (the lab's, and the directory they
  // are saved in) each window offers the pattern maker. The bridges and the
  // library must outlive the main window or be cleared first.
  void set_lasers(std::vector<LaserBridge*> bridges, bool simulation, laser::PatternLibrary* patterns = nullptr,
                  std::filesystem::path patterns_dir = {});
  QAction* laser_action() const noexcept { return laser_action_; }
  // Null until its action is first triggered.
  LaserWindow* laser_window(const QString& device) const;
  LaserWindow* open_laser(const QString& device);  // null for an unknown device
  PatternMakerWindow* pattern_maker() const noexcept { return pattern_maker_; }
  PatternMakerWindow* open_pattern_maker();        // null without a pattern library

  // Enables View > Data (browser, recall and figure windows) over `source`
  // with figure presets from `presets` (null disables it and closes every data
  // window). Both must outlive the main window or be cleared first.
  // `session_store`: `source` is the store the session saves its runs to;
  // only then does the experiment window get its References dock (not over
  // the records folder, nor a database that could not be opened).
  void set_data(processing::IAnalysisSource* source, processing::PresetStore* presets, bool session_store = false);
  // Why the data window has less than was expected (a database that could
  // not be opened): DataBrowserWindow::set_unavailable, whenever it is made.
  void set_data_unavailable(const QString& reason);
  // DataBrowserWindow::refresh_if_changed, when there is a data window (one
  // is not made for this). Called when the experiment bridge says a queue
  // started or a run finished.
  void refresh_data();
  QAction* data_action() const noexcept { return data_action_; }
  DataBrowserWindow* data_window() const noexcept { return data_->existing_browser(); }
  ProcessingBridge* processing_bridge() const noexcept { return data_->processing_bridge(); }
  // File > Installations…; hidden until a handler is set.
  void set_installations_handler(std::function<void()> handler);
  QAction* installations_action() const noexcept { return installations_; }
  // File > Preferences…. Its values are kept in the QSettings `settings` makes
  // (default: the application's); tests pass a temp file.
  void set_preferences_settings(PreferencesDialog::SettingsFactory settings);
  QAction* preferences_action() const noexcept { return preferences_; }
  // Preferences > Data source, for a window on an install: the page's
  // dependencies, read when the dialog opens, and what reopens pychron-ui on
  // the install once a new source is written. Unset: no such page.
  void set_data_source(DataSourceDepsFactory deps, PreferencesDialog::Reopen reopen);
  // The dialog, window modal; OK and Apply save and apply what it holds.
  // Over `over` (default: this window). File > Preferences… opens it over the
  // window in front, whichever one's bar it was chosen from.
  PreferencesDialog* open_preferences(QWidget* over = nullptr);
  // The line's config file. With it, Preferences has Logging and Metrics
  // pages, kept in that file's local override (line_settings.hpp); without
  // it (the default) it has neither.
  void set_line_config_file(std::filesystem::path main_file);
  // What the metrics endpoint is doing, for the Metrics page.
  void set_metrics_status(QString status);
  // Fonts (application wide) and the data browser's page size.
  void apply_preferences(const Preferences& preferences);
  // Opens a recall window / a time-series figure window (null without data).
  QWidget* open_recall(const QString& uuid);
  QWidget* open_time_series(const QStringList& uuids);
  // A figure window of a figure unit kind (time_series, ideogram, spectrum,
  // inverse_isochron, spectrum_isochron); null without data or for an unknown kind.
  QWidget* open_figure(const QString& kind, const QStringList& uuids);

  // The gauge windows' range, scale and geometry are kept in the QSettings
  // `settings` makes (default: the application's); tests pass a temp file.
  void set_gauge_settings(std::function<std::unique_ptr<QSettings>()> settings);
  // The readings of the line's gauges since the window was made.
  const ReadingHistory& reading_history() const noexcept { return history_; }
  // Opens (or brings to the front) the window of a gauge the line defines;
  // null for any other name.
  GaugeWindow* open_gauge_window(const QString& name);
  // Null until the gauge is first clicked.
  GaugeWindow* gauge_window(const QString& name) const;
  GaugePopup* gauge_popup() const noexcept { return gauge_popup_; }

  // Help > About pychron; the dialog is null until the action is first triggered.
  QAction* about_action() const noexcept { return about_action_; }
  AboutDialog* about_dialog() const { return findChild<AboutDialog*>(QString(), Qt::FindDirectChildrenOnly); }

 protected:
  // Closes the experiment window (which may refuse, keeping everything open)
  // and the spectrometer window first.
  void closeEvent(QCloseEvent* event) override;

 private:
  ExperimentWindow* ensure_experiment_window();  // made on first use; nullptr without a session
  // The experiment window's References dock, as set_data said.
  void give_references();
  ExperimentWindow* show_experiment_window();    // and brought to the front
  void build_file_entries();
  // The window in front, to open a dialog over: not a dialog itself, and
  // this window when none of the application's is active.
  QWidget* preferences_parent();
  std::unique_ptr<QSettings> spectrometer_settings() const;
  void default_layout();
  void on_pressure_sample(const PressureSample& sample);
  // The wall time of a reading taken at `ts` on the line's clock.
  QDateTime wall_of(TimePoint ts) const;

  std::unique_ptr<QSettings> settings_;  // null: the layout is not kept
  CoreBridge bridge_;
  // After the bridge: the popup and the gauge windows read it, and
  // ~MainWindow deletes them before it goes.
  ReadingHistory history_;
  CanvasView* canvas_;
  LogDock* log_;
  AlarmDock* alarms_;
  CryoDock* cryo_ = nullptr;
  HeaterDock* heaters_ = nullptr;
  HealthBar* health_;
  DockLayouts* layouts_ = nullptr;
  QAction* spectrometer_action_;
  SpectrometerBridge* spectrometer_ = nullptr;
  bool simulation_ = false;
  std::function<std::unique_ptr<QSettings>()> settings_factory_;
  SpectrometerWindow* spectrometer_window_ = nullptr;
  QAction* field_table_action_;
  FieldTableEditorWindow* field_table_window_ = nullptr;
  std::filesystem::path rise_rate_file_;
  QAction* experiment_action_;
  ExperimentBridge* experiment_ = nullptr;
  // The bridge's signals to refresh_data() and to the health bar's hold chip.
  std::array<QMetaObject::Connection, 5> experiment_refreshes_;
  bool experiment_simulation_ = false;
  std::optional<std::filesystem::path> experiment_queue_;
  std::function<std::unique_ptr<QSettings>()> experiment_settings_;
  ExperimentWindow* experiment_window_ = nullptr;
  processing::PresetStore* presets_ = nullptr;  // set_data's
  bool session_store_ = false;                  // set_data's: the experiment window gets References
  QPointer<IdentifierSource> identifier_source_;
  QList<QAction*> file_entries_;  // File > New and File > Open: live while there is a session
  QAction* data_action_;
  QAction* laser_action_;
  std::vector<LaserBridge*> lasers_;
  bool laser_simulation_ = false;
  laser::PatternLibrary* patterns_ = nullptr;
  std::filesystem::path patterns_dir_;
  std::map<QString, LaserWindow*> laser_windows_;
  PatternMakerWindow* pattern_maker_ = nullptr;
  DataWorkspace* data_;
  QAction* installations_;
  std::function<void()> on_installations_;
  QAction* about_action_ = nullptr;
  QAction* preferences_;
  PreferencesDialog::SettingsFactory preferences_settings_;
  QPointer<PreferencesDialog> preferences_dialog_;
  DataSourceDepsFactory data_source_deps_;
  PreferencesDialog::Reopen data_source_reopen_;
  std::filesystem::path line_config_file_;
  QString metrics_status_;
  GaugePopup* gauge_popup_ = nullptr;
  std::map<QString, GaugeWindow*> gauge_windows_;
  std::function<std::unique_ptr<QSettings>()> gauge_settings_;
  std::weak_ptr<LogHub> log_hub_;  // the line's, for levels that change at once
};

}  // namespace pychron::ui
