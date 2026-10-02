#include "log_model.hpp"

#include <algorithm>
#include <chrono>

namespace pychron::ui {

namespace {

QString level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Trace:
      return QStringLiteral("TRACE");
    case LogLevel::Debug:
      return QStringLiteral("DEBUG");
    case LogLevel::Info:
      return QStringLiteral("INFO");
    case LogLevel::Warn:
      return QStringLiteral("WARN");
    case LogLevel::Error:
      return QStringLiteral("ERROR");
  }
  return QStringLiteral("?");
}

}  // namespace

LogModel::LogModel(int capacity, QObject* parent)
    : QAbstractTableModel(parent), capacity_(std::max(1, capacity)) {}

void LogModel::append(std::vector<LogRecord> batch) {
  if (batch.empty()) return;
  const auto cap = static_cast<std::size_t>(capacity_);
  // Only the newest `cap` records of an oversized batch can survive.
  if (batch.size() > cap) batch.erase(batch.begin(), batch.end() - static_cast<std::ptrdiff_t>(cap));

  const std::size_t overflow = records_.size() + batch.size() > cap ? records_.size() + batch.size() - cap : 0;
  if (overflow > 0) {
    beginRemoveRows({}, 0, static_cast<int>(overflow) - 1);
    records_.erase(records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(overflow));
    endRemoveRows();
  }
  const int first = static_cast<int>(records_.size());
  beginInsertRows({}, first, first + static_cast<int>(batch.size()) - 1);
  records_.insert(records_.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
  endInsertRows();
}

void LogModel::clear() {
  beginResetModel();
  records_.clear();
  endResetModel();
}

const LogRecord& LogModel::record(int row) const { return records_.at(static_cast<std::size_t>(row)); }

int LogModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(records_.size());
}

int LogModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant LogModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
  const LogRecord& r = records_[static_cast<std::size_t>(index.row())];
  if (role == Qt::UserRole) return static_cast<int>(r.level);
  if (role != Qt::DisplayRole) return {};
  switch (index.column()) {
    case TimeCol: {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(r.ts.time_since_epoch()).count();
      return QStringLiteral("%1").arg(static_cast<double>(ms) / 1000.0, 0, 'f', 3);
    }
    case LevelCol:
      return level_name(r.level);
    case LoggerCol:
      return r.logger;
    case MessageCol:
      return r.message;
    default:
      return {};
  }
}

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  switch (section) {
    case TimeCol:
      return tr("Time");
    case LevelCol:
      return tr("Level");
    case LoggerCol:
      return tr("Logger");
    case MessageCol:
      return tr("Message");
    default:
      return {};
  }
}

}  // namespace pychron::ui
