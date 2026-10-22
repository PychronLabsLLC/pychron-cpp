#pragma once

// PeakCenterPanel (spectrometer peak center design 5.2): the named sets of a
// peak_center.toml and a form with one field per file key, to override a set
// for one run. Picking a set loads the form; a field that differs from the
// set has a bold label; revert() loads the set again. The form is checked with
// the runner's rules, and an invalid one disables Start and Save and says why.
// An unparseable file puts its error in the banner, offers the built-in
// default only and disables Save (it would overwrite what the operator has to
// fix by hand). The panel also runs: Start gives config() to the bridge's
// peak_center(), Cancel stops it, the result area shows the outcome, "Apply to
// field table" writes the fitted centre (after asking) and "Save to set…"
// writes the form as a named set of the file. The panel listens to the
// bridge's signals itself; the window only feeds the plot dock.

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include <QString>
#include <QStringList>
#include <QWidget>

#include "pychron/core/error.hpp"
#include "pychron/systems/jobs/peak_center.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace pychron::ui {

class SpectrometerBridge;

class PeakCenterPanel : public QWidget {
  Q_OBJECT

 public:
  explicit PeakCenterPanel(SpectrometerBridge& bridge, QWidget* parent = nullptr);

  // Reads the file and fills the set combo; the set picked before stays picked
  // when the file still has it. A missing file, or one without [default],
  // offers the built-in PeakCenterConfig{} as "default".
  void set_file(const std::filesystem::path& file);
  const std::filesystem::path& file() const noexcept { return file_; }

  
  // The form as a config. A field the operator has not edited keeps the picked
  // set's exact value from the file (the spin boxes round); only an edited
  // field takes the widget's value.
  jobs::PeakCenterConfig config() const;
  QString set_name() const;
  // Whether any field differs from the picked set.
  bool edited() const;
  void select_set(const QString& name);
  void revert();

  // Whether a queue is running (design 5.4): Start and Apply are then
  // disabled (tooltip "a queue is running") and Start re-checks it.
  void set_queue_running(std::function<bool()> queue_running);

  // Questions the panel asks (Apply's confirmation, overwriting a set); the
  // default is a Yes/No QMessageBox. Tests give answers.
  void set_ask(std::function<bool(QString)> ask) { ask_ = std::move(ask); }
  // The name for Save, given the default (the picked set); nullopt cancels.
  // The default is a QInputDialog.
  void set_ask_name(std::function<std::optional<QString>(QString)> ask) { ask_name_ = std::move(ask); }

  // Whether the last result can be applied now (the button's state).
  bool apply_enabled() const;

 signals:
  // Emitted just before the bridge call of a Start, so a plot can clear.
  void startRequested(pychron::jobs::PeakCenterConfig config);

 private:
  struct Field;
  void build();
  QWidget* add_field(const char* key, const QString& text, QWidget* widget);
  void load(const jobs::PeakCenterConfig& config);
  void fill_isotopes(const QString& keep);
  void refresh();
  jobs::PeakCenterConfig form_config() const;  // the widgets' values, rounded as they show
  bool file_ok() const noexcept { return banner_error_.isEmpty(); }
  bool queue_is_running() const { return queue_running_ && queue_running_(); }
  void start();
  void apply();
  void save();
  void on_finished(const Result<jobs::PeakCenterResult>& result);
  void show_result();
  void clear_result();
  bool confirm(const QString& question);

  SpectrometerBridge& bridge_;
  std::function<bool()> queue_running_;
  std::function<bool(QString)> ask_;
  std::function<std::optional<QString>(QString)> ask_name_;
  std::optional<jobs::PeakCenterResult> result_;  // the last run's result, until the next Start
  QString status_;                                // its error text, or what Apply and Save did since
  bool table_changed_ = false;                    // fieldTableChanged since result_
  bool applied_ = false;                          // Apply has written result_ (or is writing it)
  QStringList notes_;
  std::filesystem::path file_;
  std::map<std::string, jobs::PeakCenterConfig> sets_;
  QString banner_error_;
  bool loading_ = false;
  jobs::PeakCenterConfig base_;  // the form as the picked set left it (rounded by the fields)
  jobs::PeakCenterConfig exact_;  // the picked set as the file has it; config() keeps its unedited fields

  QComboBox* set_ = nullptr;
  QLabel* banner_ = nullptr;
  QLabel* reason_ = nullptr;
  QComboBox* detector_ = nullptr;
  QComboBox* isotope_ = nullptr;
  QListWidget* additional_ = nullptr;
  QCheckBox* center_enabled_ = nullptr;
  QDoubleSpinBox* center_ = nullptr;
  QDoubleSpinBox* window_ = nullptr;
  QDoubleSpinBox* step_ = nullptr;
  QDoubleSpinBox* percent_ = nullptr;
  QDoubleSpinBox* min_peak_height_ = nullptr;
  QCheckBox* test_peak_flat_ = nullptr;
  QSpinBox* n_tries_ = nullptr;
  QComboBox* direction_ = nullptr;
  QDoubleSpinBox* integration_ = nullptr;
  QDoubleSpinBox* settle_ = nullptr;
  QDoubleSpinBox* baseline_timeout_ = nullptr;
  QDoubleSpinBox* dac_offset_ = nullptr;
  QDoubleSpinBox* peak_shift_threshold_ = nullptr;
  QCheckBox* propagate_ = nullptr;
  QPushButton* start_ = nullptr;
  QPushButton* cancel_ = nullptr;
  QPushButton* revert_ = nullptr;
  QPushButton* save_ = nullptr;
  QPushButton* apply_ = nullptr;
  std::map<std::string, QLabel*> labels_;
  QLabel* res_status_ = nullptr;
  QLabel* res_initial_ = nullptr;
  QLabel* res_center_ = nullptr;
  QLabel* res_shift_ = nullptr;
  QLabel* res_table_ = nullptr;
  QLabel* res_additional_ = nullptr;
  QLabel* res_notes_ = nullptr;
};

}  // namespace pychron::ui
