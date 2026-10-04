#pragma once

// QueueTableModel (experiment-window design 5.2): the experiment queue as a
// table, revalidated against the lab after every edit, with the run status of
// each row while a queue runs.
//
// Rows mirror an ExperimentQueue; row operations go through it, so they have
// exactly its semantics. A row with an Error diagnostic is tinted red (yellow
// for warnings only) and its tooltip lists every diagnostic for that row.
// While locked nothing is editable. While live (a queue is running) the rows
// the executor has reached are read-only, and every change is first offered
// to the committer (the running session); it is kept only if accepted, else
// editRefused says why and nothing changes. The executor's own changes
// (QueueEdited) are adopted by version; the echo of an accepted edit is not.
//
// Run status is keyed by row. Neither the executor nor a live edit changes
// rows the executor has reached, so rows that already have a status never
// shift.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <QAbstractTableModel>
#include <QColor>
#include <QString>
#include <QStringList>

#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/model/experiment_queue.hpp"
#include "pychron/experiment/run/state.hpp"

namespace pychron::ui {

class QueueTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  // Status sits next to # so it stays in view on narrow windows.
  enum Column {
    Row, Status, Identifier, Aliquot, Step, Type, Position, Extract, Script, Plan, Conditionals, Comment, Estimate, Count
  };

  using Checker = std::function<experiment::lab::LabCheck(const experiment::QueueSpec&)>;
  // Offers a changed queue to the running executor: (base version, queue) -> new version.
  using Committer = std::function<Result<std::uint64_t>(std::uint64_t, const experiment::QueueSpec&)>;

  // `ids` derives a row's analysis type from its identifier; `checker`
  // validates the whole queue (ExperimentBridge::check in the app).
  QueueTableModel(experiment::IdentifierRules ids, Checker checker, QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  // Rejects (false, nothing changes) text that does not parse.
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

  // Replaces the queue (a file, or the executor's QueueEdited) and revalidates.
  // Keeps run statuses when `keep_status`.
  void set_queue(experiment::QueueSpec spec, bool keep_status = false);
  const experiment::QueueSpec& queue() const { return queue_.spec(); }
  void revalidate();
  const experiment::lab::LabCheck& check() const { return check_; }
  bool runnable() const { return check_.ok() && queue_.size() > 0; }
  // Queue-level diagnostics (run -1), one line each.
  QStringList queue_diagnostics() const;
  QStringList row_diagnostics(int row) const;
  bool row_has_error(int row) const;

  // Row operations; false (and nothing changes) when locked or rows are bad.
  // move_* returns the rows' new positions through `moved`.
  bool move_up(std::vector<std::size_t> rows, std::vector<std::size_t>* moved = nullptr);
  bool move_down(std::vector<std::size_t> rows, std::vector<std::size_t>* moved = nullptr);
  bool duplicate(std::vector<std::size_t> rows);  // copies below the last selected row
  bool remove(std::vector<std::size_t> rows);
  bool toggle_skip(std::vector<std::size_t> rows);
  bool toggle_end_after(std::size_t row);
  // Replaces one run in place (no model reset, so the selection stays).
  bool replace_run(std::size_t row, experiment::RunSpec run);
  // Inserts `runs` before row `at` (size() appends).
  bool insert_runs(std::size_t at, const std::vector<experiment::RunSpec>& runs);
  // Gives each row these conditionals files, in this order. A file a row
  // already references keeps its reference (kind included). All rows or none:
  // false when locked or a row is not editable.
  bool set_conditionals(std::vector<std::size_t> rows, const std::vector<std::string>& names);
  // The queue's conditionals file ("" for none). Refused while live: the
  // queue-wide checks are loaded when the queue starts.
  bool set_queue_conditionals(const std::string& name);
  // Expands frequency runs into the queue; how many were inserted, nullopt
  // when locked or the spec is bad.
  std::optional<std::size_t> add_frequency(const experiment::FrequencySpec& spec);

  void set_locked(bool locked);
  bool locked() const noexcept { return locked_; }

  // Live editing while a queue runs, from row `frozen` on (the start row).
  void set_live(std::size_t frozen, Committer commit);
  void end_live();
  bool live() const noexcept { return static_cast<bool>(commit_); }
  // Rows the executor has reached (QueueFrontier); only grows while live.
  void set_frozen(std::size_t rows);
  std::size_t frozen_rows() const noexcept { return live() ? frozen_ : 0; }
  std::uint64_t version() const noexcept { return version_; }
  bool row_editable(std::size_t row) const noexcept { return !locked_ && row >= frozen_rows(); }
  // Clamps an insert position so it never lands among the frozen rows.
  std::size_t insert_position(std::size_t at) const noexcept { return std::max(at, frozen_rows()); }
  // The executor changed the queue (a queue action, a post-run conditional,
  // or the echo of a live edit); adopted when newer than what the model has.
  // True when adopted.
  bool on_queue_edited(const experiment::executor::QueueEdited& e);

  // Run status, fed from the bridge.
  void clear_status();
  void on_run_started(const experiment::executor::RunStarted& e);
  void on_run_state(const experiment::run::RunStateChanged& e);
  void on_run_finished(const experiment::executor::RunFinished& e);
  QString status_text(int row) const;  // "" when the row has not run
  std::optional<experiment::run::RunState> status(int row) const;

  static QColor state_color(experiment::run::RunState state);

 signals:
  // Any change to the queue by the user (cell edit or row operation).
  void edited();
  // After each revalidation.
  void validated();
  // A live edit was refused by the executor (the queue is unchanged).
  void editRefused(const QString& why);
  // More rows became read-only while live.
  void frozenChanged();

 private:
  struct RowStatus {
    std::string run_id;
    experiment::run::RunState state = experiment::run::RunState::Pending;
    bool finished = false, truncated = false;
    int aliquot = 0;
    std::string step;
    QString error;
  };

  bool apply(const std::function<Result<void>(experiment::ExperimentQueue&)>& op);
  // Keeps `next` (the queue after an edit) if live rules and the committer
  // allow it. `reset`: the rows changed shape (else only `row` changed).
  bool adopt(experiment::ExperimentQueue next, bool reset, int row = -1);
  void row_changed(int row);
  QString text(int row, int column) const;

  experiment::IdentifierRules ids_;
  Checker checker_;
  experiment::ExperimentQueue queue_;
  experiment::lab::LabCheck check_;
  std::map<int, std::vector<experiment::Diagnostic>> by_row_;
  std::map<int, RowStatus> status_;
  std::map<std::string, int> run_rows_;  // run id -> row
  bool locked_ = false;
  Committer commit_;
  std::size_t frozen_ = 0;
  std::uint64_t version_ = 0;
};

}  // namespace pychron::ui
