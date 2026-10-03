#pragma once

// ReferenceFitWindow (data browsing and visualization design, section 7.5,
// V3 fit units): blanks or IC factors for a set of unknowns from reference
// analyses, after legacy's Blanks / ICFactor editors,
//
//   select(unknowns)   -> reduce ---------------------.
//   select(references) -> reduce -> edits (exclusions) -> blank_fit | icfactor_fit
//
// run on the ProcessingBridge. The references come from find_references
// (types, hours either side, same spectrometer / extract device); clicking a
// reference point leaves it out of the fit or puts it back. The options
// dock sets one fit per isotope (blanks) or detector pair (IC factors), with
// named presets; the References dock lists every reference with an
// Included box (the same edit as clicking its point).
// Save writes the predicted values as blanks / IC factors revisions of every
// fitted unknown in one changeset, when the source keeps revisions.

#include <QMainWindow>
#include <QStringList>
#include <QTimer>

#include "pychron/processing/options.hpp"
#include "pychron/processing/reference_fit.hpp"
#include "pychron/processing/units.hpp"
#include "processing_bridge.hpp"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace pychron::ui {

class OptionsEditor;
class PresetBar;
class SceneView;

class ReferenceFitWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `kind` is blank_fit or icfactor_fit. `bridge` and `presets` must outlive
  // the window. References are looked up on construction.
  ReferenceFitWindow(ProcessingBridge& bridge, processing::PresetStore& presets, std::string kind,
                     QStringList unknowns, QWidget* parent = nullptr);
  ~ReferenceFitWindow() override;

  static bool handles(const std::string& kind) { return kind == "blank_fit" || kind == "icfactor_fit"; }

  const std::string& kind() const noexcept { return kind_; }
  const processing::Pipeline& pipeline() const noexcept { return pipeline_; }

  // Looks up references with the finder's settings and reruns; false (the
  // reason in the status) when the lookup fails.
  bool find_references();
  void toggle_references(const QStringList& uuids);
  void set_fit_options(const processing::Options& options);
  // Saves the last fit set; false (the reason in the status) when nothing
  // was saved.
  bool save();

  // For tests.
  SceneView* view() const noexcept { return view_; }
  OptionsEditor* options_editor() const noexcept { return editor_; }
  PresetBar* presets() const noexcept { return presets_; }
  QTableWidget* references_table() const noexcept { return table_; }
  QLineEdit* reference_types() const noexcept { return types_; }
  QDoubleSpinBox* hours() const noexcept { return hours_; }
  QCheckBox* same_spectrometer() const noexcept { return same_ms_; }
  QCheckBox* same_extract_device() const noexcept { return same_device_; }
  QPushButton* save_button() const noexcept { return save_; }
  QLabel* status_label() const noexcept { return status_; }
  int runs_completed() const noexcept { return runs_; }
  const processing::ReferenceFitSetPtr& fits() const noexcept { return fits_; }
  const processing::DatasetPtr& references() const noexcept { return references_; }
  QStringList reference_uuids() const;

 signals:
  void figure_updated();
  // After a save: every saved analysis (open recall windows may reload).
  void saved(const QStringList& uuids);

 private:
  void run();
  void on_result(const PipelineResult& r);
  void update_save_state();
  void fill_table();

  ProcessingBridge& bridge_;
  processing::PresetStore& store_;
  std::string kind_;
  processing::ReferenceFitTarget target_;
  processing::SchemaPtr schema_;
  processing::Pipeline pipeline_;
  QStringList unknowns_;
  int channel_;
  int runs_ = 0;
  processing::ReferenceFitSetPtr fits_;
  processing::DatasetPtr references_;
  SceneView* view_;
  OptionsEditor* editor_;
  PresetBar* presets_;
  QTableWidget* table_;
  bool filling_ = false;
  QLineEdit* types_;
  QDoubleSpinBox* hours_;
  QCheckBox* same_ms_;
  QCheckBox* same_device_;
  QPushButton* save_;
  QLabel* status_;
  QString note_;  // the last save, shown until something changes
  QTimer debounce_;
};

}  // namespace pychron::ui
