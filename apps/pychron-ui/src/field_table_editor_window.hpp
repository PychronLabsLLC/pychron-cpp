#pragma once

// FieldTableEditorWindow (field table editor spec, section 3): the field
// tables of the spectrometer's data root.
//
//   left     table names, (active) and (HV) marked; the selected table's
//            versions, newest first, current marked
//   centre   grid of the open version (FieldTableModel)
//   right    default fit, one fit override per detector, axis, diagnostics
//            (click to go to the cell)
//
// One version is open at a time. Save writes a new version through
// SpectrometerBridge::replace_table (the core refuses while a queue or scan
// holds the table, on a conflict, or on a diagnostics error), or straight
// through FieldTableStore when there is no spectrometer. The window
// remembers the store's current version when it opened (base_version()); a
// save over a store that has moved on asks Reload / Save Anyway / Cancel.
// A version that does not parse is shown read-only with its parse error.

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QDateTime>
#include <QMainWindow>
#include <QTimer>

#include "field_table_model.hpp"
#include "field_table_plot.hpp"
#include "pychron/core/error.hpp"
#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/field_table_check.hpp"
#include "pychron/systems/spectrometer/field_table_store.hpp"
#include "pychron/systems/spectrometer/molecular_weights.hpp"

class QAction;
class QComboBox;
class QDialog;
class QFormLayout;
class QLabel;
class QListWidget;
class QTableView;
class QToolBar;
class QTreeWidget;
class QVBoxLayout;

namespace pychron::ui {

class FieldTableAddRowDialog;
class FieldTableSetValueDialog;
class SpectrometerBridge;

class FieldTableEditorWindow : public QMainWindow {
  Q_OBJECT

 public:
  enum class Unsaved { Save, Discard, Cancel };
  enum class Conflict { Reload, SaveAnyway, Cancel };
  static constexpr int kCheckDelayMs = 300;
  static constexpr int kHoldPollMs = 500;

  // `bridge` null: no spectrometer; saves go straight to FieldTableStore,
  // stamped with `wall_clock`. A bridge must outlive the window.
  FieldTableEditorWindow(SpectrometerBridge* bridge, std::filesystem::path data_root,
                         spectrometer::cfg::SpectrometerConfig config, spectrometer::MolecularWeights weights,
                         std::function<QDateTime()> wall_clock, QWidget* parent = nullptr);
  ~FieldTableEditorWindow() override;
  FieldTableEditorWindow(const FieldTableEditorWindow&) = delete;
  FieldTableEditorWindow& operator=(const FieldTableEditorWindow&) = delete;

  // Opens the table's current version (asks about unsaved edits first).
  void open_table(const QString& name);
  void open_version(const QString& name, const QString& version);
  const QString& open_name() const { return open_name_; }
  const QString& open_version() const { return open_version_; }
  const QString& base_version() const { return base_version_; }
  FieldTableModel* model() noexcept { return &model_; }
  // Follows the grid's current detector column.
  FieldTablePlot* plot() noexcept { return plot_; }
  bool modified() const { return model_.modified(); }
  bool read_only() const { return model_.read_only(); }
  const QString& load_error() const { return load_error_; }

  QStringList table_names() const;
  QStringList versions() const;  // of the open table, newest first

  // Diagnostics now, skipping the debounce.
  void check_now();
  const spectrometer::TableDiagnostics& diagnostics() const noexcept { return diagnostics_; }
  QStringList diagnostic_lines() const;

  void save();
  void make_current(const QString& version);
  void revert();
  bool save_enabled() const { return save_disabled_reason().isEmpty(); }
  QString save_disabled_reason() const;
  bool make_current_enabled(const QString& version) const { return make_current_disabled_reason(version).isEmpty(); }
  QString make_current_disabled_reason(const QString& version) const;
  const QString& banner_text() const { return banner_text_; }

  // Compare (spec 3.4): `v1` against `v2`, or against the open edits when
  // `v2` is empty. A side that does not parse is compared as text. The
  // dialog is shown, not run, and deletes itself when closed.
  QDialog* compare(const QString& v1, const QString& v2);
  // Import CSV (spec 3.5): the picked legacy mftable.csv opens as an unsaved
  // edit of the open table's current version. A read error goes to
  // show_error.
  void import_csv();
  using PickCsv = std::function<std::optional<std::filesystem::path>()>;
  void set_pick_csv(PickCsv pick) { pick_csv_ = std::move(pick); }
  // Set Value… and Add Row…: the toolbar runs the dialog, then applies it.
  std::unique_ptr<FieldTableSetValueDialog> make_set_value_dialog();
  void apply_set_value(const FieldTableSetValueDialog& dialog);
  std::unique_ptr<FieldTableAddRowDialog> make_add_row_dialog();
  void apply_add_row(const FieldTableAddRowDialog& dialog);

  // Dialog hooks; defaults are message boxes. Tests replace them.
  void set_ask_unsaved(std::function<Unsaved(const QString&)> ask) { ask_unsaved_ = std::move(ask); }
  void set_ask_conflict(std::function<Conflict(const QString& now_version)> ask) { ask_conflict_ = std::move(ask); }
  // Comments lost, Make Current.
  void set_confirm(std::function<bool(const QString& question)> confirm) { confirm_ = std::move(confirm); }
  void set_show_error(std::function<void(const QString&)> show) { show_error_ = std::move(show); }

 signals:
  void saved(const QString& name, const QString& version);

 protected:
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;  // ActivationChange: reread the lists

 private:
  spectrometer::FieldTableStore store(const QString& name) const;
  std::filesystem::path version_file(const QString& name, const QString& version) const;
  // false: stay where we are. A Save answer saves first and then runs
  // `then` (the switch or close that asked).
  bool resolve_unsaved(std::function<void()> then);
  // Loads without asking about unsaved edits.
  void load_version(const QString& name, const QString& version);
  void load_current(const QString& name);
  void load_current_saved(const QString& name);  // without the in-memory check
  void fill_tables();
  void fill_versions();
  void rebuild_fit_combos();
  void sync_fit_combos();
  void update_actions();
  void update_title();
  void set_banner(const QString& text);
  void redraw_plot();
  void start_save(const QString& base);
  void finish_save(const QString& name, const Result<std::string>& result);
  void finish_restore(const QString& name, const QString& version, const Result<void>& result);
  void on_table_changed(const QString& name, const QString& version);
  void on_conflict(const QString& now);
  std::optional<QString> held() const;
  // A listed version, parsed, and its first §3.2 error once asked for.
  struct VersionVerdict {
    std::optional<spectrometer::FieldTable> table;  // nullopt: does not parse
    QString parse_error;
    mutable std::optional<QString> first_error;  // empty string: none
  };
  const VersionVerdict& verdict(const QString& version) const;
  // The store's current version of the open table; empty when unknown.
  QString store_current() const;

  SpectrometerBridge* bridge_;
  std::filesystem::path data_root_;
  spectrometer::cfg::SpectrometerConfig config_;
  std::function<QDateTime()> wall_clock_;
  FieldTableModel model_;

  QString open_name_;
  QString open_version_;
  QString base_version_;
  QString listed_current_;  // current version when the list was last read
  QString load_error_;
  QString banner_text_;
  bool has_comments_ = false;        // the open version's text is not canonical
  std::set<QString> comments_asked_;  // "<name>/<version>"
  spectrometer::TableDiagnostics diagnostics_;
  bool save_pending_ = false;
  bool restore_pending_ = false;  // a Make Current is on the bridge's executor
  mutable std::map<QString, VersionVerdict> verdicts_;  // of the open table, since the last listing
  int saving_index_ = 0;                // undo index of what the pending save writes
  spectrometer::FieldTable saving_table_;
  std::function<void()> after_save_;
  bool filling_ = false;
  std::vector<std::string> fit_combo_columns_;  // the detectors rebuild_fit_combos made a combo for

  QListWidget* tables_;
  QTreeWidget* versions_;
  QTableView* grid_;
  QLabel* banner_;
  QLabel* load_error_label_;
  QComboBox* default_fit_;
  QFormLayout* overrides_;
  QLabel* axis_;
  QListWidget* diagnostics_list_;
  FieldTablePlot* plot_;
  QToolBar* toolbar_ = nullptr;
  QVBoxLayout* centre_layout_ = nullptr;
  QAction* save_action_;
  QAction* revert_action_;
  QAction* make_current_action_;
  QAction* remove_row_action_;
  QAction* add_row_action_ = nullptr;
  QAction* set_value_action_ = nullptr;
  QAction* compare_action_ = nullptr;
  QAction* import_action_ = nullptr;
  QAction* undo_action_;
  QAction* redo_action_;
  QTimer check_timer_;
  QTimer hold_timer_;

  std::function<Unsaved(const QString&)> ask_unsaved_;
  std::function<Conflict(const QString&)> ask_conflict_;
  std::function<bool(const QString&)> confirm_;
  std::function<void(const QString&)> show_error_;
  PickCsv pick_csv_;
};

}  // namespace pychron::ui
