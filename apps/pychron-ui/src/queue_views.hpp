#pragma once

// The Executed and Queue views (queue-tables design 3): two filters over the
// one QueueTableModel, split at its executed boundary. Rows never move in the
// model; only a change of the boundary (executedChanged) refilters, so a
// run's status change stays a dataChanged. No sorting: rows keep queue order.

#include <QSortFilterProxyModel>

namespace pychron::ui {

class QueueTableModel;

// One side of the executed boundary.
class QueueSplitProxy : public QSortFilterProxyModel {
  Q_OBJECT

 protected:
  // `executed`: the side below executed_rows(), else the side from it on.
  QueueSplitProxy(QueueTableModel& source, bool executed, QObject* parent);
  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

 private:
  void refilter();

  const QueueTableModel* source_;
  bool executed_;
};

// Source rows below executed_rows(); nothing in it is editable.
class ExecutedProxy : public QueueSplitProxy {
  Q_OBJECT

 public:
  explicit ExecutedProxy(QueueTableModel& source, QObject* parent = nullptr);
  Qt::ItemFlags flags(const QModelIndex& index) const override;
};

// Source rows from executed_rows() on.
class QueueProxy : public QueueSplitProxy {
  Q_OBJECT

 public:
  explicit QueueProxy(QueueTableModel& source, QObject* parent = nullptr);
};

}  // namespace pychron::ui
