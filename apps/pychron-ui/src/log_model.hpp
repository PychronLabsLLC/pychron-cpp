#pragma once

// LogModel: bounded ring of log records (columns Time, Level, Logger,
// Message) for the log view. Qt::UserRole on any cell yields the level (int).

#include <vector>

#include <QAbstractTableModel>
#include <QDateTime>
#include <QString>

#include "pychron/core/clock.hpp"
#include "pychron/core/events.hpp"

namespace pychron::ui {

// "TRACE" .. "ERROR".
QString log_level_name(LogLevel level);

// Maps a steady_clock TimePoint to local wall time using a steady->system
// anchor captured once (on first use, at start-up).
QDateTime log_wall_time(TimePoint ts);

struct LogRecord {
  TimePoint ts;
  LogLevel level;
  QString logger;
  QString message;
  bool history = false;
};

class LogModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  static constexpr int kCapacity = 10000;
  enum Column { TimeCol = 0, LevelCol, LoggerCol, MessageCol, ColumnCount };

  explicit LogModel(int capacity = kCapacity, QObject* parent = nullptr);

  // Appends records, evicting the oldest beyond capacity.
  void append(std::vector<LogRecord> batch);
  void clear();
  const LogRecord& record(int row) const;

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

 private:
  int capacity_;
  std::vector<LogRecord> records_;
};

}  // namespace pychron::ui
