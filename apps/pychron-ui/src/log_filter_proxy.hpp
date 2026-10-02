#pragma once

// LogFilterProxy: level / logger-pattern / message-text filter over LogModel.

#include <QSortFilterProxyModel>
#include <QString>

#include "pychron/core/events.hpp"

namespace pychron::ui {

class LogFilterProxy : public QSortFilterProxyModel {
  Q_OBJECT

 public:
  explicit LogFilterProxy(QObject* parent = nullptr);

  void set_min_level(LogLevel level);
  // Empty = all loggers. Otherwise pychron::log_name_matches (a bare name
  // includes its dotted children; '*' makes it a glob).
  void set_logger_pattern(const QString& pattern);
  // Case-insensitive substring of the message.
  void set_text(const QString& text);

 protected:
  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

 private:
  LogLevel min_level_ = LogLevel::Trace;
  QString logger_pattern_;
  QString text_;
};

}  // namespace pychron::ui
