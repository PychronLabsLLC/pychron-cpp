#include "log_filter_proxy.hpp"

#include "log_model.hpp"
#include "pychron/core/log_match.hpp"

namespace pychron::ui {

// begin/endFilterChange arrived in Qt 6.10 (and deprecate invalidateRowsFilter);
// Ubuntu 24.04 ships 6.4.
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#define PYCHRON_FILTER_CHANGE(...)                          \
  do {                                                       \
    beginFilterChange();                                     \
    __VA_ARGS__;                                             \
    endFilterChange(QSortFilterProxyModel::Direction::Rows); \
  } while (0)
#else
#define PYCHRON_FILTER_CHANGE(...) \
  do {                             \
    __VA_ARGS__;                   \
    invalidateRowsFilter();        \
  } while (0)
#endif

LogFilterProxy::LogFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {}

void LogFilterProxy::set_min_level(LogLevel level) {
  PYCHRON_FILTER_CHANGE(min_level_ = level);
}

void LogFilterProxy::set_logger_pattern(const QString& pattern) {
  PYCHRON_FILTER_CHANGE(logger_pattern_ = pattern.toStdString());
}

void LogFilterProxy::set_text(const QString& text) {
  PYCHRON_FILTER_CHANGE(text_ = text);
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
