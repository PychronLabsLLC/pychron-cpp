#include "log_filter_proxy.hpp"

#include "log_model.hpp"
#include "pychron/core/log_match.hpp"

namespace pychron::ui {

LogFilterProxy::LogFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {}

void LogFilterProxy::set_min_level(LogLevel level) {
  beginFilterChange();
  min_level_ = level;
  endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void LogFilterProxy::set_logger_pattern(const QString& pattern) {
  beginFilterChange();
  logger_pattern_ = pattern.toStdString();
  endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void LogFilterProxy::set_text(const QString& text) {
  beginFilterChange();
  text_ = text;
  endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool LogFilterProxy::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
  const auto* model = qobject_cast<const LogModel*>(sourceModel());
  if (model == nullptr || source_parent.isValid()) return true;
  const LogRecord& r = model->record(source_row);
  if (static_cast<int>(r.level) < static_cast<int>(min_level_)) return false;
  if (!logger_pattern_.empty() && !log_name_matches(logger_pattern_, r.logger.toStdString())) return false;
  if (!text_.isEmpty() && !r.message.contains(text_, Qt::CaseInsensitive)) return false;
  return true;
}

}  // namespace pychron::ui
