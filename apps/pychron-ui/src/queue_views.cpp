#include "queue_views.hpp"

#include "queue_table_model.hpp"

namespace pychron::ui {

QueueSplitProxy::QueueSplitProxy(QueueTableModel& source, bool executed, QObject* parent)
    : QSortFilterProxyModel(parent), source_(&source), executed_(executed) {
  setDynamicSortFilter(false);
  setSourceModel(&source);
  connect(&source, &QueueTableModel::executedChanged, this, [this] { refilter(); });
}

bool QueueSplitProxy::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
  if (source_parent.isValid() || source_row < 0) return false;
  return (static_cast<std::size_t>(source_row) < source_->executed_rows()) == executed_;
}

// begin/endFilterChange arrived in Qt 6.10 (and deprecate invalidateFilter and
// invalidateRowsFilter); Ubuntu 24.04 ships 6.4. The boundary has already
// moved when executedChanged arrives, so there is nothing between the two.
void QueueSplitProxy::refilter() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  beginFilterChange();
  endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
  invalidateRowsFilter();
#endif
}

ExecutedProxy::ExecutedProxy(QueueTableModel& source, QObject* parent) : QueueSplitProxy(source, true, parent) {}

Qt::ItemFlags ExecutedProxy::flags(const QModelIndex& index) const {
  return QSortFilterProxyModel::flags(index) & ~Qt::ItemIsEditable;
}

QueueProxy::QueueProxy(QueueTableModel& source, QObject* parent) : QueueSplitProxy(source, false, parent) {}

}  // namespace pychron::ui
