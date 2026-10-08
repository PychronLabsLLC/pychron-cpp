#pragma once

// The flux window (flux window design, sections 5.3 and 5.4): the irradiation
// levels on the left, and for the one picked the plot of its monitors over the
// tables of monitors, of the selected monitor's analyses and of the unknowns.
// A level is read from the store on the bridge's worker and fitted here, on the
// GUI thread; the window computes nothing itself (the scene, the status line
// and the warnings are libs/processing's).

#include <optional>
#include <string>

#include <QMainWindow>
#include <QString>
#include <QStringList>

#include "entry_bridge.hpp"
#include "pychron/processing/flux_fit.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"

class QLabel;
class QTableView;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace pychron::ui {

class FluxAnalysisModel;
class FluxMonitorModel;
class FluxUnknownModel;
class SceneView;

class FluxWindow : public QMainWindow {
  Q_OBJECT

 public:
  // All three must outlive the window. A level is read through `source` on the
  // bridge's worker, so `source` must also outlive the jobs the window started:
  // destroy the bridge (which finishes them) before the source.
  FluxWindow(EntryBridge& bridge, processing::IAnalysisSource& source, processing::PresetStore& presets,
             QWidget* parent = nullptr);

  void reload_tree();                                                  // asynchronous
  void open_level(const QString& irradiation, const QString& level);  // asynchronous
  // A bridge job is running, or a refit is pending.
  bool busy() const noexcept;
  QString status() const;
  bool status_is_error() const noexcept { return status_error_; }
  QStringList warnings() const { return warnings_; }  // the status tooltip, one per line

  QTreeWidget* tree() const noexcept { return tree_; }
  SceneView* view() const noexcept { return view_; }
  FluxMonitorModel* monitors() const noexcept { return monitors_; }
  FluxUnknownModel* unknowns() const noexcept { return unknowns_; }
  FluxAnalysisModel* analyses() const noexcept { return analyses_; }
  QTableView* monitor_table() const noexcept { return monitor_table_; }
  const processing::LevelInputs* inputs() const noexcept { return inputs_ ? &*inputs_ : nullptr; }  // nullptr until a level loaded
  const processing::LevelFit* fit() const noexcept { return fit_ ? &*fit_ : nullptr; }  // nullptr when the fit failed
  const processing::FluxOptions& options() const noexcept { return options_; }
  void select_monitor(int hole);  // as clicking its row

 private:
  void start_load();                              // of irradiation_ / level_
  void apply_loaded(processing::LevelInputs inputs);
  void clear_level();                             // nothing on show; the models let go of inputs_ and fit_
  void request_fit();                             // fit_now() 150 ms after the last request
  void fit_now();
  void show_selected();                           // the analyses table, of selected_hole_
  void update_scene();
  void update_title();
  void set_status(const QString& text, bool error, const QStringList& warnings = {});
  void set_tables_enabled(bool enabled);
  QTreeWidgetItem* tree_item(const QString& irradiation, const QString& level) const;
  void select_tree_item();                        // the item of irradiation_ / level_, without loading it

  EntryBridge& bridge_;
  processing::IAnalysisSource& source_;
  [[maybe_unused]] processing::PresetStore& presets_;  // the preset bar's; nothing reads it yet

  QTreeWidget* tree_ = nullptr;
  SceneView* view_ = nullptr;
  FluxMonitorModel* monitors_ = nullptr;
  FluxUnknownModel* unknowns_ = nullptr;
  FluxAnalysisModel* analyses_ = nullptr;
  QTableView* monitor_table_ = nullptr;
  QTableView* analysis_table_ = nullptr;
  QTableView* unknown_table_ = nullptr;
  QLabel* status_ = nullptr;
  QTimer* fit_timer_ = nullptr;

  // The level asked for (empty: none yet), and what was read and made of it.
  // The models point into inputs_, fit_ and unfitted_: they are reset before
  // any of the three changes.
  QString irradiation_, level_;
  std::optional<processing::LevelInputs> inputs_;
  processing::FluxOptions options_;
  processing::Edits edits_;
  std::optional<processing::LevelFit> fit_;
  std::string fit_error_;
  std::optional<int> selected_hole_;
  // The selected monitor when there is no fit: its analyses as fit_level would have counted them.
  std::optional<processing::FittedPosition> unfitted_;

  int busy_ = 0;       // bridge jobs outstanding
  int tree_jobs_ = 0;  // of them, tree reads
  quint64 load_generation_ = 0, tree_generation_ = 0;  // a result of an older request is dropped
  bool resetting_ = false;  // the monitors table is being reset: its selection signals are not the user's
  bool status_error_ = false;
  QStringList warnings_;
};

}  // namespace pychron::ui
