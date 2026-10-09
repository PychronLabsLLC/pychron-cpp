#pragma once

// ExperimentWindow (experiment-window design 5.5): the queue table in the
// center, the run factory and the selected run's measurement on the left
// (tabbed), the executor pane docked below and the evolutions on the right.
//
// The queue is edited in place and saved to its TOML file; every edit is
// revalidated against the lab. While a queue runs the rows the executor has
// not reached stay editable (each change goes to the executor first) and the
// table follows the executor (run status per row, and the executor's own
// queue edits). Closing asks about unsaved edits (Cancel keeps the window open)
// and, while running, whether to stop the queue; the queue keeps running if
// the window is only hidden.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <QList>
#include <QMainWindow>
#include <QSettings>
#include <QString>

#include "dock_layouts.hpp"

#include "conditionals_editor_window.hpp"
#include "evolutions_view.hpp"
#include "executor_pane.hpp"
#include "experiment_bridge.hpp"
#include "queue_table_model.hpp"
#include "measurement_panel.hpp"
#include "run_factory_panel.hpp"
#include "script_editor_window.hpp"

class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QTableView;
class QToolBar;

namespace pychron::ui {

class ExperimentWindow : public QMainWindow {
  Q_OBJECT

 public:
  enum class Unsaved { Save, Discard, Cancel };

  // `bridge` must outlive the window. `settings` defaults to the
  // application's QSettings; tests pass a temp file.
  ExperimentWindow(ExperimentBridge& bridge, bool simulation, std::unique_ptr<QSettings> settings = nullptr,
                   QWidget* parent = nullptr);

  QueueTableModel& model() noexcept { return model_; }
  // Where the panels are: the factory layout, the last one, the named ones.
  DockLayouts* dock_layouts() const noexcept { return layouts_; }
  QTableView* table() const noexcept { return table_; }
  ExecutorPane* executor() const noexcept { return pane_; }
  EvolutionsView* evolutions() const noexcept { return evolutions_; }
  RunFactoryPanel* factory() const noexcept { return factory_; }
  MeasurementPanel* measurement() const noexcept { return measurement_; }
  // Null until first opened.
  ScriptEditorWindow* script_editor() const noexcept { return script_editor_; }
  ScriptEditorWindow* open_script_editor();
  // Opens the selected row's script of `kind` (extraction or post-measurement);
  // false without one selected row or script.
  bool edit_row_script(scripting::ScriptKind kind);

  // The conditionals editor (conditionals-editor design 6.2). Null until first
  // opened; `file` is opened when named and present.
  ConditionalsEditorWindow* conditionals_editor() const noexcept { return conditionals_editor_; }
  ConditionalsEditorWindow* open_conditionals_editor(const QString& file = {});
  // The queue's conditionals file: "(none)" and the lab's files.
  QComboBox* queue_conditionals_combo() const noexcept { return queue_conditionals_; }
  // Asks which conditionals files the selected rows get and applies the
  // answer; false without a selection, on cancel, or when a row cannot change.
  bool edit_selected_conditionals();
  // The question: the files and, per file, whether all (Checked), none
  // (Unchecked) or some (PartiallyChecked) of the rows have it. The answer is
  // the states wanted; a file left PartiallyChecked stays as each row has it.
  using PickConditionals =
      std::function<std::optional<QList<Qt::CheckState>>(const QStringList& names, const QList<Qt::CheckState>& states)>;
  void set_pick_conditionals(PickConditionals pick) { pick_conditionals_ = std::move(pick); }

  // Replaces the queue; refused (false, with `error`) while running or when
  // the file does not parse. Asks about unsaved edits first.
  bool load_queue(const std::filesystem::path& path, QString* error = nullptr);
  // Empties the queue and forgets its file. Refused (false, with `error`)
  // while a queue runs. Asks about unsaved edits first.
  bool new_queue(QString* error = nullptr);
  bool save(QString* error = nullptr);  // to the current path; false if none
  bool save_as(const std::filesystem::path& path, QString* error = nullptr);
  const std::optional<std::filesystem::path>& path() const noexcept { return path_; }
  bool modified() const noexcept { return modified_; }
  QString queue_diagnostics_text() const;

  // Starts at the selected row (row 0 without a selection).
  void start();
  void select_row(int row);

  // Dialog hooks; defaults are message boxes. Tests replace them.
  void set_confirm(ExecutorPane::Confirm confirm);
  void set_ask_unsaved(std::function<Unsaved()> ask) { ask_unsaved_ = std::move(ask); }
  void set_ask_stop(std::function<bool()> ask) { ask_stop_ = std::move(ask); }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void build_actions();
  std::vector<std::size_t> selected_rows() const;
  void select_rows(const std::vector<std::size_t>& rows);
  bool resolve_unsaved();
  void set_modified(bool modified);
  void default_layout();
  void update_title();
  void update_state();
  void open_dialog();
  void save_as_dialog();
  void sync_queue_conditionals();  // the combo follows the queue and the lab's files
  std::optional<QList<Qt::CheckState>> pick_conditionals_dialog(const QStringList& names,
                                                                const QList<Qt::CheckState>& states);

  ExperimentBridge& bridge_;
  bool simulation_;
  std::unique_ptr<QSettings> settings_;
  DockLayouts* layouts_ = nullptr;
  QDockWidget* executor_dock_ = nullptr;
  QDockWidget* evolutions_dock_ = nullptr;
  QDockWidget* factory_dock_ = nullptr;
  QDockWidget* measurement_dock_ = nullptr;
  QToolBar* toolbar_ = nullptr;
  QueueTableModel model_;
  QTableView* table_;
  QLabel* diagnostics_;
  ExecutorPane* pane_;
  EvolutionsView* evolutions_;
  RunFactoryPanel* factory_;
  MeasurementPanel* measurement_;
  ScriptEditorWindow* script_editor_ = nullptr;
  ConditionalsEditorWindow* conditionals_editor_ = nullptr;
  QComboBox* queue_conditionals_ = nullptr;
  PickConditionals pick_conditionals_;
  std::optional<std::filesystem::path> path_;
  bool modified_ = false;
  std::function<Unsaved()> ask_unsaved_;
  std::function<bool()> ask_stop_;

  QAction* new_ = nullptr;
  QAction* open_ = nullptr;
  QAction* save_ = nullptr;
  QAction* save_as_ = nullptr;
  QAction* revalidate_ = nullptr;
  std::vector<QAction*> row_actions_;
};

}  // namespace pychron::ui
