#pragma once

// FigureWindow (data browsing and visualization design, section 11.4): a
// figure pipeline (time series, ideogram, spectrum, inverse isochron) for a
// set of analyses,
//
//   select(uuids) -> reduce -> group -> edits -> figure
//
// run on the ProcessingBridge. The scene is drawn by SceneView; the options
// dock is generated from the figure's schema and backed by named presets;
// the analyses dock shows each analysis's group and whether it is included.
// Clicking a point (or toggling it in the table) edits the `edits` unit's
// options and reruns; only edits and the figure recompute.

#include <functional>
#include <set>

#include <QMainWindow>
#include <QStringList>
#include <QTimer>

#include "pychron/processing/options.hpp"
#include "pychron/processing/units.hpp"
#include "processing_bridge.hpp"

class QComboBox;
class QLabel;
class QTableWidget;

namespace pychron::ui {

class OptionsEditor;
class PresetBar;
class SceneView;

class FigureWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `bridge` and `presets` must outlive the window. `kind` is a figure unit
  // kind: time_series, ideogram, spectrum, inverse_isochron or spectrum_isochron.
  FigureWindow(ProcessingBridge& bridge, processing::PresetStore& presets, std::string kind, QStringList uuids,
               QWidget* parent = nullptr);
  // A time series.
  FigureWindow(ProcessingBridge& bridge, processing::PresetStore& presets, QStringList uuids,
               QWidget* parent = nullptr);

  const std::string& kind() const noexcept { return kind_; }
  // Group key a new window of `kind` starts with (aliquot for spectra and
  // isochrons, identifier for ideograms, none for time series).
  static QString default_group_key(const std::string& kind);
  ~FigureWindow() override;

  const processing::Pipeline& pipeline() const noexcept { return pipeline_; }
  // Applies new figure options (as the dock does) and reruns.
  void set_figure_options(const processing::Options& options);
  void set_group_key(const QString& key);
  // Which groups share a graph: "none" (all on one), "same as group" (a graph
  // per group) or a key of the group unit, when the groups of one sample, say,
  // are to be drawn together. The graphs are laid out by the figure's
  // "Graphs per row".
  void set_graph_key(const QString& key);
  static QString same_as_group();
  void toggle_exclusion(const QStringList& uuids);
  void select_preset(const QString& name);
  bool export_figure(const QString& path);  // .pdf or .png by extension
  // The figure's analyses as a Schaen et al. (2021) data report (.json, else
  // CSV), with this window's grouping and exclusions; a spectrum's plateau
  // options decide the plateau. False before the first run or when the file
  // cannot be written.
  bool export_report(const QString& path);

  // Dialog hooks for tests: the name for "Save as" (empty: cancelled).
  std::function<QString()> ask_preset_name;

  // For tests.
  SceneView* view() const noexcept { return view_; }
  OptionsEditor* options_editor() const noexcept { return editor_; }
  QComboBox* preset_combo() const noexcept;
  QComboBox* group_combo() const noexcept { return group_; }
  QComboBox* graph_combo() const noexcept { return graph_; }
  QTableWidget* analyses_table() const noexcept { return analyses_; }
  QLabel* status_label() const noexcept { return status_; }
  int runs_completed() const noexcept { return runs_; }
  const processing::DatasetPtr& dataset() const noexcept { return dataset_; }

 signals:
  void recall_requested(const QString& uuid);
  void figure_updated();

 private:
  void run();
  void schedule();
  void on_result(const PipelineResult& r);
  void fill_analyses();
  void apply_graph_key();  // the graph unit's key from the two combos

  ProcessingBridge& bridge_;
  processing::PresetStore& store_;
  std::string kind_;
  processing::SchemaPtr schema_;
  int channel_;
  processing::Pipeline pipeline_;
  processing::DatasetPtr dataset_;
  SceneView* view_;
  OptionsEditor* editor_;
  PresetBar* presets_;
  QComboBox* group_;
  QComboBox* graph_;
  QTableWidget* analyses_;
  QLabel* status_;
  QTimer debounce_;
  int runs_ = 0;
  bool quantities_set_ = false;
  bool filling_ = false;
};

}  // namespace pychron::ui
