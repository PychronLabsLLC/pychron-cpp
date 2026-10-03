#pragma once

// IsotopeEvolutionWindow (data browsing and visualization design, section
// 7.5, V3 fit units): batch isotope-evolution refits, after legacy's Fit
// IsoEvo editor,
//
//   select(analyses) -> reduce -> edits (exclusions) -> isotope_evolution_fit
//
// run on the ProcessingBridge. The summary scene shows each isotope's
// refitted and current intercepts against run time (flagged refits marked;
// click a point to leave its analysis out). The Analyses dock lists every
// analysis with an Included box, its goodness flags and refits; selecting
// one previews its signal or baseline evolutions with the new fits. Save writes the refits as
// intercepts revisions of every refitted analysis in one changeset (or of
// the good ones only), when the source keeps revisions.
//
// The isotope classifier is trained here: Good / Bad under the preview add
// the previewed analysis's chosen isotope (its sniff) to the training file
// (classifier_file, under the user's config directory by default); the Fits
// dock's "Classify sniffs" turns its flags on.

#include <filesystem>

#include <QMainWindow>
#include <QStringList>
#include <QTimer>

#include "pychron/processing/isotope_evolution_fit.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/units.hpp"
#include "processing_bridge.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTabBar;
class QTableWidget;

namespace pychron::ui {

class OptionsEditor;
class PresetBar;
class SceneView;

class IsotopeEvolutionWindow : public QMainWindow {
  Q_OBJECT

 public:
  static constexpr const char* kKind = "isotope_evolution_fit";

  // `bridge` and `presets` must outlive the window.
  IsotopeEvolutionWindow(ProcessingBridge& bridge, processing::PresetStore& presets, QStringList uuids,
                         QWidget* parent = nullptr);
  ~IsotopeEvolutionWindow() override;

  const processing::Pipeline& pipeline() const noexcept { return pipeline_; }
  void toggle_exclusion(const QStringList& uuids);
  void set_fit_options(const processing::Options& options);
  // Shows the evolutions of `uuid` with its refits; false when it was not refitted.
  bool preview(const QString& uuid);
  // Saves the last refits (the good ones only when leave_out_flagged()
  // is checked); false (the reason in the status) when nothing was saved.
  bool save();

  // The classifier training file (sets the fit options' classifier_file).
  void set_classifier_file(const std::filesystem::path& file);
  const std::filesystem::path& classifier_file() const noexcept { return classifier_file_; }
  // Adds the previewed analysis's isotope `key` as a good (1) or bad (0)
  // training sample; false (the reason in the status) when it cannot.
  bool train(const QString& key, int klass);

  // For tests.
  SceneView* view() const noexcept { return view_; }
  SceneView* preview_view() const noexcept { return preview_; }
  QTabBar* preview_kind() const noexcept { return preview_kind_; }  // signals, baselines
  QComboBox* train_isotope() const noexcept { return train_isotope_; }
  QLabel* training_label() const noexcept { return training_; }
  OptionsEditor* options_editor() const noexcept { return editor_; }
  PresetBar* presets() const noexcept { return presets_; }
  QTableWidget* analyses_table() const noexcept { return table_; }
  QCheckBox* leave_out_flagged() const noexcept { return only_good_; }
  QPushButton* save_button() const noexcept { return save_; }
  QLabel* status_label() const noexcept { return status_; }
  int runs_completed() const noexcept { return runs_; }
  const processing::IsotopeFitSetPtr& fits() const noexcept { return fits_; }

 signals:
  void figure_updated();
  void saved(const QStringList& uuids);

 private:
  void run();
  void on_result(const PipelineResult& r);
  void fill_table();
  void update_save_state();
  const processing::AnalysisRefits* refits_of(const std::string& uuid) const;
  void update_training_state();

  ProcessingBridge& bridge_;
  processing::PresetStore& store_;
  processing::SchemaPtr schema_;
  processing::Pipeline pipeline_;
  int channel_;
  int runs_ = 0;
  processing::IsotopeFitSetPtr fits_;
  processing::DatasetPtr dataset_;
  SceneView* view_;
  SceneView* preview_;
  QTabBar* preview_kind_;
  QComboBox* train_isotope_;
  QPushButton* train_good_;
  QPushButton* train_bad_;
  QLabel* training_;
  std::filesystem::path classifier_file_;
  int training_version_ = 0;
  OptionsEditor* editor_;
  PresetBar* presets_;
  QTableWidget* table_;
  QCheckBox* only_good_;
  QPushButton* save_;
  QLabel* status_;
  QString note_;
  QString previewed_;
  bool filling_ = false;
  QTimer debounce_;
};

}  // namespace pychron::ui
