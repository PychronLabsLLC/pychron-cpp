#include "log_model.hpp"

#include <algorithm>
#include <chrono>

#include <QBrush>
#include <QColor>

namespace pychron::ui {

QString log_level_name(LogLevel level) {
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

namespace {

struct WallAnchor {
  std::chrono::steady_clock::time_point steady = std::chrono::steady_clock::now();
  std::chrono::system_clock::time_point system = std::chrono::system_clock::now();
};

const WallAnchor& wall_anchor() {
  static const WallAnchor anchor;
  return anchor;
}

}  // namespace

QDateTime log_wall_time(TimePoint ts) {
  using namespace std::chrono;
  const WallAnchor& anchor = wall_anchor();
  const auto wall = anchor.system + duration_cast<system_clock::duration>(ts - anchor.steady);
  const auto ms = duration_cast<milliseconds>(wall.time_since_epoch()).count();
  return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms));
}

TimePoint log_steady_time(const QDateTime& wall) {
  using namespace std::chrono;
  const WallAnchor& anchor = wall_anchor();
  const system_clock::time_point sys{duration_cast<system_clock::duration>(milliseconds(wall.toMSecsSinceEpoch()))};
  return anchor.steady + duration_cast<steady_clock::duration>(sys - anchor.system);
}

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
  if (role == Qt::ForegroundRole) {
    if (r.history) return QBrush(QColor(Qt::gray));
    return {};
  }
  if (role != Qt::DisplayRole) return {};
  switch (index.column()) {
    case TimeCol:
      return log_wall_time(r.ts).toString(QStringLiteral("HH:mm:ss.zzz"));
    case LevelCol:
      return log_level_name(r.level);
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
