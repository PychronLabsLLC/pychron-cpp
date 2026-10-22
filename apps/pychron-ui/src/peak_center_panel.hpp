#pragma once

// PeakCenterPanel (spectrometer peak center design 5.2): the named sets of a
// peak_center.toml and a form with one field per file key, to override a set
// for one run. Picking a set loads the form; a field that differs from the
// set has a bold label; revert() loads the set again. The form is checked with
// the runner's rules, and an invalid one disables Start and Save and says why.
// An unparseable file puts its error in the banner, offers the built-in
// default only and disables Save (it would overwrite what the operator has to
// fix by hand). The file is only read here. Running, results, Apply and Save
// are wired by the window that holds the panel.

#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include <QString>
#include <QWidget>

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

  // The form as a config, named for the picked set. update_table is the set's.
  jobs::PeakCenterConfig config() const;
  QString set_name() const;
  // Whether any field differs from the picked set.
  bool edited() const;
  void select_set(const QString& name);
  void revert();

  // Stored for the queue exclusion (design 5.4); not acted on yet.
  void set_queue_running(std::function<bool()> queue_running) { queue_running_ = std::move(queue_running); }

 signals:
  void startRequested(pychron::jobs::PeakCenterConfig config);

 private:
  struct Field;
  void build();
  QWidget* add_field(const char* key, const QString& text, QWidget* widget);
  void load(const jobs::PeakCenterConfig& config);
  void fill_isotopes(const QString& keep);
  void refresh();
  bool file_ok() const noexcept { return banner_error_.isEmpty(); }

  SpectrometerBridge& bridge_;
  std::function<bool()> queue_running_;
  std::filesystem::path file_;
  std::map<std::string, jobs::PeakCenterConfig> sets_;
  QString banner_error_;
  bool loading_ = false;
  jobs::PeakCenterConfig base_;  // the form as the picked set left it (rounded by the fields)
  bool update_table_ = true;     // the picked set's, kept for config()

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
};

}  // namespace pychron::ui
