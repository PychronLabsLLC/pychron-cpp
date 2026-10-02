#pragma once

// ExperimentWindow (experiment-window design 5.5): the queue table in the
// centre, the run factory on the left, the executor pane docked below and the
// evolutions on the right.
//
// The queue is edited in place and saved to its TOML file; every edit is
// revalidated against the lab. While a queue runs the table is locked and
// follows the executor (run status per row, and the executor's own queue
// edits). Closing asks about unsaved edits (Cancel keeps the window open)
// and, while running, whether to stop the queue; the queue keeps running if
// the window is only hidden.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <QMainWindow>
#include <QSettings>
#include <QString>

#include "evolutions_view.hpp"
#include "executor_pane.hpp"
#include "experiment_bridge.hpp"
#include "queue_table_model.hpp"
#include "run_factory_panel.hpp"

class QAction;
class QLabel;
class QTableView;

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
  QTableView* table() const noexcept { return table_; }
  ExecutorPane* executor() const noexcept { return pane_; }
  EvolutionsView* evolutions() const noexcept { return evolutions_; }
  RunFactoryPanel* factory() const noexcept { return factory_; }

  // Replaces the queue; refused (false, with `error`) while running or when
  // the file does not parse. Asks about unsaved edits first.
  bool load_queue(const std::filesystem::path& path, QString* error = nullptr);
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
  void update_title();
  void update_state();
  void open_dialog();
  void save_as_dialog();

  ExperimentBridge& bridge_;
  bool simulation_;
  std::unique_ptr<QSettings> settings_;
  QueueTableModel model_;
  QTableView* table_;
  QLabel* diagnostics_;
  ExecutorPane* pane_;
  EvolutionsView* evolutions_;
  RunFactoryPanel* factory_;
  std::optional<std::filesystem::path> path_;
  bool modified_ = false;
  std::function<Unsaved()> ask_unsaved_;
  std::function<bool()> ask_stop_;

  QAction* open_ = nullptr;
  QAction* save_ = nullptr;
  QAction* save_as_ = nullptr;
  QAction* revalidate_ = nullptr;
  std::vector<QAction*> row_actions_;
};

}  // namespace pychron::ui
