#pragma once

// ExperimentWindow (experiment-window design 5.5): the queue's two tables in
// the center (queue-tables design 3: Executed above, the runs not yet executed
// in Queue below, one model split at its executed boundary), the run factory and the selected run's measurement on the left
// (tabbed), the executor pane docked below and the evolutions on the right.
//
// The queue is edited in place and saved to its TOML file; every edit is
// revalidated against the lab. While a queue runs the rows the executor has
// not reached stay editable (each change goes to the executor first) and the
// table follows the executor (run status per row, and the executor's own
// queue edits). Closing asks about unsaved edits (Cancel keeps the window open)
// and, while running, whether to stop the queue; the queue keeps running if
// the window is only hidden.
//
// Given the data workspace's ProcessingBridge and figure presets
// (set_references), it has a References dock beside the evolutions: the
// recent blanks and airs of the queue's spectrometer, read again after each
// saved run (docs/superpowers/specs/2026-10-13-experiment-reference-series-design.md).

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QList>
#include <QMainWindow>
#include <QSettings>
#include <QString>
#include <QTimer>

#include "dock_layouts.hpp"

#include "conditionals_editor_window.hpp"
#include "evolutions_view.hpp"
#include "executor_pane.hpp"
#include "experiment_bridge.hpp"
#include "pychron/core/clock.hpp"
#include "queue_table_model.hpp"
#include "queue_views.hpp"
#include "resume_offer.hpp"
#include "recent_queues.hpp"
#include "measurement_panel.hpp"
#include "run_factory_panel.hpp"
#include "script_editor_window.hpp"

class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QMenu;
class QSplitter;
class QTableView;
class QToolBar;
class QToolButton;
class QWidget;

namespace pychron::processing {
class PresetStore;
}

namespace pychron::ui {

class ProcessingBridge;
class ReferenceSeriesDock;

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
  // The runs not executed yet: edited here, and what row actions act on.
  QTableView* queue_table() const noexcept { return queue_table_; }
  // The runs executed (read-only), above the Queue table.
  QTableView* executed_table() const noexcept { return executed_table_; }
  // Its header row (fold button, "Executed (N)") and executed_table(); hidden
  // while nothing is executed.
  QWidget* executed_section() const noexcept { return executed_section_; }
  QSplitter* tables_splitter() const noexcept { return tables_splitter_; }
  // Collapsed, the Executed section shows its header only. Kept in QSettings.
  bool executed_collapsed() const noexcept { return executed_collapsed_; }
  void set_executed_collapsed(bool collapsed);
  ExecutorPane* executor() const noexcept { return pane_; }
  // Executor > Resume and Skip Held Run..., enabled only while the queue holds.
  QAction* resume_held_action() const noexcept { return resume_held_; }
  QAction* skip_held_action() const noexcept { return skip_held_; }
  EvolutionsView* evolutions() const noexcept { return evolutions_; }
  RunFactoryPanel* factory() const noexcept { return factory_; }
  MeasurementPanel* measurement() const noexcept { return measurement_; }

  // The References dock (reference-series design 5.1), made when both are
  // given and taken away when either is null. Both must outlive the dock:
  // call set_references(nullptr, nullptr) before either goes. Given only
  // when the data source is the store the session saves to.
  void set_references(ProcessingBridge* bridge, processing::PresetStore* presets);
  // Null without one.
  ReferenceSeriesDock* references() const noexcept { return references_; }
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
  // File > New Queue and File > Open Queue…: new_queue(), and load_queue()
  // of a file asked for; each says so when a queue is running.
  void new_dialog();
  void open_dialog();
  // File > Save Queue As… (and Save of a queue with no file yet).
  void save_as_dialog();
  bool save(QString* error = nullptr);  // to the current path; false if none
  bool save_as(const std::filesystem::path& path, QString* error = nullptr);
  const std::optional<std::filesystem::path>& path() const noexcept { return path_; }
  // File > Open Recent: the queues opened or saved last, newest first, then
  // Clear Menu. Rebuilt when shown and after each change of the list; its
  // action is greyed while the list is empty.
  QMenu* recent_menu() const noexcept { return recent_menu_; }
  bool modified() const noexcept { return modified_; }
  QString queue_diagnostics_text() const;

  // Starts at the first row not executed (model().executed_rows()); the
  // selection is not read.
  void start();
  // Selects source row `row` in the Queue table, if it is there (an executed
  // row is not).
  void select_row(int row);

  // Queue > Reset (queue-tables design 4.1): every run back in the Queue
  // table, the queue unchanged. False while running ("a queue is running").
  bool reset_executed(QString* error = nullptr);
  // Queue > Clear Executed... (4.2): asks first, then removes the executed
  // runs from the queue. False while running ("a queue is running"), with
  // none executed ("no run has been executed"), on no ("cancelled"), or when
  // the queue cannot change.
  bool clear_executed(QString* error = nullptr);
  // Queue > Repeat Last Run (4.3): copies of the rows selected in the
  // Executed table, else of its last row, at the front of the Queue table;
  // allowed while running. False with none executed ("no run has been
  // executed") or when the edit is refused.
  bool repeat_last(QString* error = nullptr);
  // The question Clear Executed asks, given how many runs it removes;
  // default: a message box.
  void set_ask_clear_executed(std::function<bool(int runs)> ask) { ask_clear_executed_ = std::move(ask); }

  // Resume after a crash or a Stop (queue-tables design 5): reads the
  // session's <data>/executor_state.json and, when resume_offer() makes an
  // offer, asks; on yes (and resumable) the executed boundary and the earlier
  // statuses come from the file and the queue starts. False while running,
  // with "a queue is running", before any file is read. False with `error`
  // left empty when nothing is offered: the window just stopped
  // (executed_rows() > 0), no file, a file that does not read, no offer.
  bool offer_resume(QString* error = nullptr);
  // The question; default: a dialog, Resume / Not now (Resume disabled when
  // the offer is not resumable).
  void set_ask_resume(std::function<bool(const ResumeOffer&)> ask) { ask_resume_ = std::move(ask); }
  // How a Queue menu action shows its refusal; default: a message box.
  void set_inform(std::function<void(const QString&)> inform) { inform_ = std::move(inform); }
  // Queue > Resume...
  QAction* resume_action() const noexcept { return resume_; }

  // Dialog hooks; defaults are message boxes. Tests replace them.
  void set_confirm(ExecutorPane::Confirm confirm);
  void set_ask_unsaved(std::function<Unsaved()> ask) { ask_unsaved_ = std::move(ask); }
  void set_ask_stop(std::function<bool()> ask) { ask_stop_ = std::move(ask); }
  // The file asked for by Open and Save As, given the directory or file the
  // dialog starts at (queue-directory design, section 3); empty on cancel.
  // Defaults are the file dialogs.
  using PickFile = std::function<QString(const QString& start)>;
  void set_pick_open_file(PickFile pick) { pick_open_file_ = std::move(pick); }
  void set_pick_save_file(PickFile pick) { pick_save_file_ = std::move(pick); }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void build_actions();
  std::vector<std::size_t> selected_rows() const;
  // Source rows of the Executed table's selection.
  std::vector<std::size_t> selected_executed_rows() const;
  // Opens `row`'s script of `kind` in the script editor; false without one.
  bool open_row_script(std::size_t row, scripting::ScriptKind kind);
  // A Queue menu action's refusal, through inform_; "cancelled" says nothing.
  void inform_refusal(const QString& error);
  bool ask_resume_dialog(const ResumeOffer& offer);
  void select_rows(const std::vector<std::size_t>& rows);
  bool resolve_unsaved();
  void set_modified(bool modified);
  void default_layout();
  // The References dock reads again when the queue's spectrometer is no
  // longer `before`.
  void references_follow(const std::string& before);
  // Calls `what` once, `delay` later on the session's clock, on this thread.
  void later(std::chrono::seconds delay, std::function<void()> what);
  void run_due();
  void update_title();
  void update_state();
  // The Executed section follows the boundary: its count, whether it shows,
  // and its newest row unless the operator scrolled up.
  void on_executed_changed();
  void rebuild_recent();
  void open_recent(const QString& file);
  void sync_queue_conditionals();  // the combo follows the queue and the lab's files
  std::optional<QList<Qt::CheckState>> pick_conditionals_dialog(const QStringList& names,
                                                                const QList<Qt::CheckState>& states);

  ExperimentBridge& bridge_;
  bool simulation_;
  std::unique_ptr<QSettings> settings_;
  RecentQueues recent_;  // on *settings_
  DockLayouts* layouts_ = nullptr;
  QDockWidget* executor_dock_ = nullptr;
  QDockWidget* evolutions_dock_ = nullptr;
  QDockWidget* factory_dock_ = nullptr;
  QDockWidget* measurement_dock_ = nullptr;
  QDockWidget* references_dock_ = nullptr;
  ReferenceSeriesDock* references_ = nullptr;
  struct Later {
    TimePoint at;
    std::function<void()> what;
  };
  std::vector<Later> later_;
  QTimer later_clock_;  // looks at the session's clock while something waits in later_
  QToolBar* toolbar_ = nullptr;
  QueueTableModel model_;
  QueueProxy queue_proxy_{model_};
  ExecutedProxy executed_proxy_{model_};
  QTableView* queue_table_;
  QTableView* executed_table_ = nullptr;
  QWidget* executed_section_ = nullptr;
  QWidget* executed_header_ = nullptr;
  QToolButton* executed_fold_ = nullptr;
  QLabel* executed_count_ = nullptr;
  QSplitter* tables_splitter_ = nullptr;
  bool executed_collapsed_ = false;
  bool executed_follow_ = true;  // the Executed table's scroll bar was at its end
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
  std::function<bool(int)> ask_clear_executed_;
  std::function<bool(const ResumeOffer&)> ask_resume_;
  std::function<void(const QString&)> inform_;
  PickFile pick_open_file_;
  PickFile pick_save_file_;

  QAction* open_ = nullptr;
  QAction* save_ = nullptr;
  QAction* save_as_ = nullptr;
  QAction* revalidate_ = nullptr;
  QAction* resume_ = nullptr;       // Queue > Resume...
  QMenu* recent_menu_ = nullptr;
  QAction* resume_held_ = nullptr;  // Executor > Resume
  QAction* skip_held_ = nullptr;    // Executor > Skip Held Run...
  std::vector<QAction*> row_actions_;
};

}  // namespace pychron::ui
