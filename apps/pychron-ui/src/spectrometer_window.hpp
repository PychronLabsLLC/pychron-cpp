#pragma once

// SpectrometerWindow (spectrometer-window design sections 5.4, 5.6, 6): the
// live strip chart with a "Controls" dock (integration, graph, detectors,
// magnet), an "Intensities" dock and a banner for scan and command errors.
// A "Peak center" dock (tabbed with Controls) runs peak centers and a
// "Peak center plot" dock draws them (spectrometer peak center design 5).
//
// The window talks only to SpectrometerBridge: readings and state arrive as
// signals on the GUI thread, and every command is non-blocking. Showing the
// window starts the scan; closing it stops the scan and saves the settings
// under `spectrometer_window/<spectrometer name>`. Saved values are read
// defensively: anything unknown, non-numeric or inverted falls back to the
// default.

#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
#include <vector>

#include <QMainWindow>
#include <QSettings>
#include <QString>

#include "dock_layouts.hpp"
#include <QTimer>

#include "intensities_model.hpp"
#include "peak_center_panel.hpp"
#include "peak_center_view.hpp"
#include "rise_rate_panel.hpp"
#include "spectrometer_bridge.hpp"
#include "strip_chart_model.hpp"
#include "strip_chart_view.hpp"

class QCheckBox;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;

namespace pychron::ui {

class SpectrometerWindow : public QMainWindow {
  Q_OBJECT

 public:
  static constexpr double kDefaultConfirmMoveAmu = 5.0;
  static constexpr int kRefreshMs = 50;

  // `bridge` must outlive the window. `settings` defaults to the
  // application's QSettings; tests pass a temp file.
  SpectrometerWindow(SpectrometerBridge& bridge, bool simulation, std::unique_ptr<QSettings> settings = nullptr,
                     QWidget* parent = nullptr);

  StripChartModel& chart_model() { return model_; }
  // Where the panels are: the factory layout, the last one, the named ones.
  DockLayouts* dock_layouts() const noexcept { return layouts_; }
  StripChartView* chart_view() const { return view_; }
  IntensitiesModel* intensities() const { return intensities_; }
  QTableView* intensities_view() const { return intensities_view_; }
  RiseRatePanel* rise_rate() const { return rise_; }
  // The wall time a rise rate result is stamped with (the line's clock, so a
  // simulated session is stamped in simulated time). Not given: no stamp.
  void set_wall_clock(std::function<QDateTime()> now);
  // The CSV file each rise rate result is appended to (empty: none).
  void set_rise_rate_file(const std::filesystem::path& path);

  // The peak_center.toml the Peak center dock reads its sets from and saves
  // to (empty: the built-in default only; read at once and after each save).
  void set_peak_center_file(const std::filesystem::path& file);
  // Whether a queue is running: the dock's Start and Apply are then disabled.
  void set_queue_running(std::function<bool()> queue_running);
  bool peak_center_running() const { return bridge_.peak_center_running(); }
  PeakCenterPanel* peak_center_panel() const noexcept { return peak_panel_; }
  PeakCenterView* peak_center_view() const noexcept { return peak_view_; }
  // The question asked when the window is closed during a peak center
  // ("Cancel peak center?"). Default: a Yes/No QMessageBox, default No.
  void set_ask(std::function<bool(QString)> ask) { ask_ = std::move(ask); }

  // Asked before a large move with the mass change on the reference detector
  // (NaN when the current mass is unknown). Default: Yes/No dialog, default No.
  void set_confirm_move(std::function<bool(double delta_amu)> confirm);
  // Moves larger than this (amu) ask first; 0 never asks. The setter
  // (File > Preferences…) saves it for this spectrometer at once; a negative
  // or non-finite value is ignored.
  double confirm_move_amu() const noexcept { return confirm_move_amu_; }
  void set_confirm_move_amu(double amu);
  // The same value in `settings` for spectrometer `name` while no window is
  // open (the default when unset or invalid).
  static double saved_confirm_move_amu(QSettings& settings, const QString& name);
  static void save_confirm_move_amu(QSettings& settings, const QString& name, double amu);

  // Programmatic equivalents of the controls, used by tests. Unknown detector
  // or isotope names are ignored.
  void set_detector_shown(const QString& detector, bool shown);
  void select_target(const QString& detector, const QString& isotope);
  void apply_position();  // the Apply button
  void choose_integration(double seconds);
  void set_scan_width_minutes(double minutes);
  void set_scale(YScale scale);
  void set_autoscale(bool on);
  // The Min/Max fields; false (and the fields revert) unless lo < hi, and
  // lo > 0 on the log scale. Takes effect while autoscale is off.
  bool set_manual_y(double lo, double hi);
  void clear_chart();
  QString banner_text() const;  // empty when hidden
  void restart_scan();          // the banner's Restart button
  QString position_text() const;
  QString mass_text() const;
  QString actual_integration_text() const;
  bool apply_enabled() const;

 protected:
  void showEvent(QShowEvent* event) override;    // start scan at saved or default integration
  void closeEvent(QCloseEvent* event) override;  // stop scan, save settings

 private:
  QWidget* build_controls();
  void load_settings();
  void save_settings();

  int detector_index(const QString& detector) const;  // -1 when unknown
  void show_detector(int index, bool shown);          // chart and table together
  double integration_s() const;
  void select_integration(double seconds);
  void fill_isotopes();
  void default_layout();
  void edit_manual_y();
  void sync_y_fields();
  void update_banner();
  void update_magnet_labels();
  void update_actual_integration();
  void update_rise_marks();  // the rise rate run's start and stop on the chart
  void forget_rise_marks();
  void on_readings(const std::vector<spectrometer::IntensityReading>& batch);
  void on_command_finished(const QString& what, const Result<void>& result);

  SpectrometerBridge& bridge_;
  bool simulation_;
  std::unique_ptr<QSettings> settings_;
  DockLayouts* layouts_ = nullptr;
  QDockWidget* controls_dock_ = nullptr;
  QDockWidget* intensities_dock_ = nullptr;
  QDockWidget* peak_dock_ = nullptr;
  QDockWidget* peak_plot_dock_ = nullptr;
  PeakCenterPanel* peak_panel_ = nullptr;
  PeakCenterView* peak_view_ = nullptr;
  std::function<bool(QString)> ask_;
  bool closing_after_peak_center_ = false;  // closeEvent asked to cancel; close when the run has finished
  StripChartModel model_;
  StripChartView* view_;
  IntensitiesModel* intensities_;
  QTableView* intensities_view_ = nullptr;
  RiseRatePanel* rise_ = nullptr;
  std::optional<TimePoint> rise_start_, rise_stop_;

  QFrame* banner_;
  QLabel* banner_label_;
  QComboBox* integration_ = nullptr;
  QLabel* actual_integration_ = nullptr;
  QDoubleSpinBox* scan_width_ = nullptr;
  QComboBox* scale_ = nullptr;
  QCheckBox* autoscale_ = nullptr;
  QLineEdit* ymax_ = nullptr;
  QLineEdit* ymin_ = nullptr;
  QComboBox* target_detector_ = nullptr;
  QComboBox* target_isotope_ = nullptr;
  QPushButton* apply_ = nullptr;
  QLabel* position_ = nullptr;
  QLabel* mass_ = nullptr;

  QTimer refresh_;
  bool dirty_ = true;          // the model changed since the last refresh
  bool move_pending_ = false;
  QString command_error_;      // last failed command, until the next success
  double confirm_move_amu_ = kDefaultConfirmMoveAmu;  // 0 disables
  std::function<bool(double)> confirm_move_;
};

}  // namespace pychron::ui
