#pragma once

// M1 status/control window (spec section 10.3): canvas in the centre, log and
// alarm docks, per-transport health chips in the status bar. Owns the
// CoreBridge; the ExtractionLine must outlive the window.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <QAction>
#include <QList>
#include <QIcon>
#include <QMainWindow>
#include <QPointer>
#include <QSettings>

#include "alarm_dock.hpp"
#include "brand.hpp"
#include "data_browser_window.hpp"
#include "data_workspace.hpp"
#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "experiment_window.hpp"
#include "health_bar.hpp"
#include "log_dock.hpp"
#include "preferences_dialog.hpp"
#include "processing_bridge.hpp"
#include "spectrometer_window.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::ui {

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(systems::ExtractionLine& line, QWidget* parent = nullptr);

  // The glyph beside each View menu item: a line drawing, a mask icon the
  // platform colours to suit its menus.
  enum class View { ExtractionLine, Spectrometer, Experiment, Data };
  static QIcon view_icon(View view);
  // Closes the data windows before the processing bridge they use goes.
  ~MainWindow() override;

  CoreBridge& bridge() noexcept { return bridge_; }
  CanvasView* canvas_view() const noexcept { return canvas_; }
  LogDock* log_dock() const noexcept { return log_; }
  AlarmDock* alarm_dock() const noexcept { return alarms_; }
  HealthBar* health_bar() const noexcept { return health_; }

  // Enables View > Spectrometer for `bridge` (null disables it and closes
  // the window). The bridge must outlive the main window or be cleared first.
  // `settings` makes the window's QSettings (default: the application's);
  // tests pass a temp file.
  void set_spectrometer(SpectrometerBridge* bridge, bool simulation,
                        std::function<std::unique_ptr<QSettings>()> settings = {});
  QAction* spectrometer_action() const noexcept { return spectrometer_action_; }
  // Null until the action is first triggered.
  SpectrometerWindow* spectrometer_window() const noexcept { return spectrometer_window_; }

  // Enables View > Experiment for `bridge` (null disables it and closes the
  // window). `queue` is opened the first time the window shows. Same
  // lifetime and settings rules as set_spectrometer.
  void set_experiment(ExperimentBridge* bridge, bool simulation, std::optional<std::filesystem::path> queue = {},
                      std::function<std::unique_ptr<QSettings>()> settings = {});
  QAction* experiment_action() const noexcept { return experiment_action_; }
  ExperimentWindow* experiment_window() const noexcept { return experiment_window_; }

  // Enables View > Data (browser, recall and figure windows) over `source`
  // with figure presets from `presets` (null disables it and closes every data
  // window). Both must outlive the main window or be cleared first.
  void set_data(processing::IAnalysisSource* source, processing::PresetStore* presets);
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
  // The dialog, window modal; OK and Apply save and apply what it holds.
  // Over `over` (default: this window). File > Preferences… opens it over the
  // window in front, whichever one's bar it was chosen from.
  PreferencesDialog* open_preferences(QWidget* over = nullptr);
  // Fonts (application wide) and the data browser's page size.
  void apply_preferences(const Preferences& preferences);
  // Opens a recall window / a time-series figure window (null without data).
  QWidget* open_recall(const QString& uuid);
  QWidget* open_time_series(const QStringList& uuids);
  // A figure window of a figure unit kind (time_series, ideogram, spectrum,
  // inverse_isochron); null without data or for an unknown kind.
  QWidget* open_figure(const QString& kind, const QStringList& uuids);

  // Help > About pychron; the dialog is null until the action is first triggered.
  QAction* about_action() const noexcept { return about_action_; }
  AboutDialog* about_dialog() const { return findChild<AboutDialog*>(QString(), Qt::FindDirectChildrenOnly); }

 protected:
  // Closes the experiment window (which may refuse, keeping everything open)
  // and the spectrometer window first.
  void closeEvent(QCloseEvent* event) override;

 private:
  // The window in front, to open a dialog over: not a dialog itself, and
  // this window when none of the application's is active.
  QWidget* preferences_parent();
  std::unique_ptr<QSettings> spectrometer_settings() const;

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
  QAction* data_action_;
  DataWorkspace* data_;
  QAction* installations_;
  std::function<void()> on_installations_;
  QAction* about_action_ = nullptr;
  QAction* preferences_;
  PreferencesDialog::SettingsFactory preferences_settings_;
  QPointer<PreferencesDialog> preferences_dialog_;
};

}  // namespace pychron::ui
