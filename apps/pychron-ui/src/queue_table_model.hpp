#pragma once

// QueueTableModel (experiment-window design 5.2): the experiment queue as a
// table, revalidated against the lab after every edit, with the run status of
// each row while a queue runs.
//
// Rows mirror an ExperimentQueue; row operations go through it, so they have
// exactly its semantics. A row with an Error diagnostic is tinted red (yellow
// for warnings only) and its tooltip lists every diagnostic for that row.
// While locked (a queue is running) nothing is editable.
//
// Run status is keyed by row. The executor only inserts rows after the last
// started one, so rows that already have a status never shift.

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
  enum Column { Row, Identifier, Aliquot, Step, Type, Position, Extract, Script, Plan, Comment, Estimate, Status, Count };

  using Checker = std::function<experiment::lab::LabCheck(const experiment::QueueSpec&)>;

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

  void set_locked(bool locked);
  bool locked() const noexcept { return locked_; }

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
};

}  // namespace pychron::ui
