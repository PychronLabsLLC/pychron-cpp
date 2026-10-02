#include "queue_table_model.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QBrush>
#include <QFont>

#include "pychron/experiment/model/positions.hpp"

namespace pychron::ui {

namespace {

using experiment::QueueSpec;
using experiment::RunSpec;
using experiment::Severity;
using experiment::run::RunState;

QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString clock_text(experiment::Duration d) {
  const auto total = static_cast<long long>(std::llround(d.count()));
  return QStringLiteral("%1:%2:%3")
      .arg(total / 3600)
      .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
      .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// "5", "5 w", "5w", "12.5 %": value and optional unit.
std::optional<std::pair<double, std::optional<experiment::Unit>>> parse_extract(const QString& text) {
  const QString t = text.trimmed();
  if (t.isEmpty()) return std::make_pair(0.0, std::optional<experiment::Unit>{});
  qsizetype split = 0;
  while (split < t.size() && (t[split].isDigit() || t[split] == QLatin1Char('.') || t[split] == QLatin1Char('-') ||
                              t[split] == QLatin1Char('+') || t[split] == QLatin1Char('e')))
    ++split;
  bool ok = false;
  const double value = t.left(split).toDouble(&ok);
  if (!ok || !std::isfinite(value)) return std::nullopt;
  const QString unit = t.mid(split).trimmed();
  if (unit.isEmpty()) return std::make_pair(value, std::optional<experiment::Unit>{});
  auto u = experiment::parse_unit(unit.toLower().toStdString());
  if (!u) return std::nullopt;
  return std::make_pair(value, u);
}

}  // namespace

QueueTableModel::QueueTableModel(experiment::IdentifierRules ids, Checker checker, QObject* parent)
    : QAbstractTableModel(parent), ids_(std::move(ids)), checker_(std::move(checker)) {}

int QueueTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(queue_.size());
}

int QueueTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : Count; }

QString QueueTableModel::text(int row, int column) const {
  const RunSpec& r = queue_.runs()[static_cast<std::size_t>(row)];
  const auto st = status_.find(row);
  switch (column) {
    case Row: return QString::number(row);
    case Identifier: return q(r.id.identifier);
    case Aliquot:
      if (r.id.aliquot) return QString::number(*r.id.aliquot);
      if (st != status_.end() && st->second.aliquot > 0) return QString::number(st->second.aliquot);
      return {};
    case Step: return q(r.id.step);
    case Type: return q(experiment::to_string(r.id.type));
    case Position: return r.extraction.position ? q(experiment::format_position(*r.extraction.position)) : QString();
    case Extract:
      if (r.extraction.value == 0) return {};
      return QStringLiteral("%1 %2").arg(QString::number(r.extraction.value, 'g', 6), q(experiment::to_string(r.extraction.units)));
    case Script: return q(r.extraction.script);
    case Plan: return q(r.measurement.plan);
    case Comment: return q(r.comment);
    case Estimate: {
      const auto& est = check_.report.run_estimates;
      if (r.skip || static_cast<std::size_t>(row) >= est.size()) return {};
      return clock_text(est[static_cast<std::size_t>(row)]);
    }
    case Status: return status_text(row);
    default: return {};
  }
}

QVariant QueueTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= rowCount() || index.column() >= Count) return {};
  const int row = index.row();
  const RunSpec& r = queue_.runs()[static_cast<std::size_t>(row)];
  switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole: return text(row, index.column());
    case Qt::ToolTipRole: {
      QStringList lines = row_diagnostics(row);
      if (index.column() == Status) {
        if (auto st = status_.find(row); st != status_.end() && !st->second.error.isEmpty())
          lines.prepend(st->second.error);
      }
      if (r.skip) lines.append(tr("skipped"));
      if (r.end_after) lines.append(tr("the queue ends after this run"));
      return lines.isEmpty() ? QVariant() : QVariant(lines.join(QLatin1Char('\n')));
    }
    case Qt::BackgroundRole: {
      if (index.column() == Status) {
        if (auto s = status(row)) return QBrush(state_color(*s));
        return {};
      }
      auto it = by_row_.find(row);
      if (it == by_row_.end()) return {};
      const bool error = std::any_of(it->second.begin(), it->second.end(),
                                     [](const auto& d) { return d.severity == Severity::Error; });
      return QBrush(error ? QColor(0xf8, 0xd7, 0xda) : QColor(0xff, 0xf3, 0xcd));
    }
    case Qt::ForegroundRole:
      if (r.skip) return QBrush(QColor(0x88, 0x88, 0x88));
      return {};
    case Qt::FontRole:
      if (r.end_after && index.column() == Identifier) {
        QFont f;
        f.setBold(true);
        return f;
      }
      return {};
    default: return {};
  }
}

QVariant QueueTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* names[Count] = {"#",        "Status",  "Identifier", "Aliquot", "Step",    "Type",
                                     "Position", "Extract", "Script",     "Plan",    "Comment", "Est."};
  return section >= 0 && section < Count ? tr(names[section]) : QVariant();
}

Qt::ItemFlags QueueTableModel::flags(const QModelIndex& index) const {
  if (!index.isValid()) return Qt::NoItemFlags;
  Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
  if (locked_) return f;
  switch (index.column()) {
    case Identifier:
    case Position:
    case Extract:
    case Script:
    case Plan:
    case Comment: return f | Qt::ItemIsEditable;
    default: return f;
  }
}

bool QueueTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (role != Qt::EditRole || !index.isValid() || locked_ || !(flags(index) & Qt::ItemIsEditable)) return false;
  const auto row = static_cast<std::size_t>(index.row());
  RunSpec r = queue_.runs()[row];
  const QString text = value.toString();
  const std::string s = text.trimmed().toStdString();
  switch (index.column()) {
    case Identifier:
      r.id.identifier = s;
      r.id.type = ids_.classify(s);
      break;
    case Position:
      if (s.empty()) {
        r.extraction.position.reset();
      } else {
        auto p = experiment::parse_position(s);
        if (!p) return false;
        r.extraction.position = *p;
      }
      break;
    case Extract: {
      auto e = parse_extract(text);
      if (!e) return false;
      r.extraction.value = e->first;
      if (e->second) r.extraction.units = *e->second;
      break;
    }
    case Script: r.extraction.script = s; break;
    case Plan: r.measurement.plan = s; break;
    case Comment: r.comment = text.toStdString(); break;
    default: return false;
  }
  if (r == queue_.runs()[row]) return true;
  if (!queue_.replace(row, std::move(r))) return false;
  revalidate();
  emit edited();
  return true;
}

void QueueTableModel::set_queue(QueueSpec spec, bool keep_status) {
  beginResetModel();
  queue_ = experiment::ExperimentQueue(std::move(spec));
  if (!keep_status) {
    status_.clear();
    run_rows_.clear();
  }
  endResetModel();
  revalidate();
}

void QueueTableModel::revalidate() {
  check_ = checker_ ? checker_(queue_.spec()) : experiment::lab::LabCheck{};
  by_row_.clear();
  for (const auto& d : check_.all())
    if (d.run >= 0) by_row_[d.run].push_back(d);
  if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, Count - 1));
  emit validated();
}

QStringList QueueTableModel::queue_diagnostics() const {
  QStringList out;
  for (const auto& d : check_.all())
    if (d.run < 0)
      out.append((d.severity == Severity::Error ? tr("error: ") : tr("warning: ")) + q(experiment::lab::describe(d)));
  return out;
}

QStringList QueueTableModel::row_diagnostics(int row) const {
  QStringList out;
  if (auto it = by_row_.find(row); it != by_row_.end())
    for (const auto& d : it->second)
      out.append((d.severity == Severity::Error ? tr("error: ") : tr("warning: ")) + q(d.field) +
                 QStringLiteral(": ") + q(d.message));
  return out;
}

bool QueueTableModel::row_has_error(int row) const {
  auto it = by_row_.find(row);
  return it != by_row_.end() &&
         std::any_of(it->second.begin(), it->second.end(), [](const auto& d) { return d.severity == Severity::Error; });
}

bool QueueTableModel::apply(const std::function<Result<void>(experiment::ExperimentQueue&)>& op) {
  if (locked_) return false;
  experiment::ExperimentQueue copy = queue_;
  if (!op(copy)) return false;
  beginResetModel();
  queue_ = std::move(copy);
  endResetModel();
  revalidate();
  emit edited();
  return true;
}

bool QueueTableModel::move_up(std::vector<std::size_t> rows, std::vector<std::size_t>* moved) {
  if (rows.empty()) return false;
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  if (rows.front() == 0) return false;
  const std::size_t to = rows.front() - 1;
  if (!apply([&](auto& qq) { return qq.move(rows, to); })) return false;
  if (moved) {
    moved->clear();
    for (std::size_t i = 0; i < rows.size(); ++i) moved->push_back(to + i);
  }
  return true;
}

bool QueueTableModel::move_down(std::vector<std::size_t> rows, std::vector<std::size_t>* moved) {
  if (rows.empty()) return false;
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  if (rows.back() + 1 >= queue_.size()) return false;
  const std::size_t to = rows.back() + 2;
  if (!apply([&](auto& qq) { return qq.move(rows, to); })) return false;
  if (moved) {
    moved->clear();
    const std::size_t first = to - rows.size();
    for (std::size_t i = 0; i < rows.size(); ++i) moved->push_back(first + i);
  }
  return true;
}

bool QueueTableModel::duplicate(std::vector<std::size_t> rows) {
  if (rows.empty()) return false;
  const std::size_t to = *std::max_element(rows.begin(), rows.end()) + 1;
  return apply([&](auto& qq) { return qq.copy(rows, to); });
}

bool QueueTableModel::remove(std::vector<std::size_t> rows) {
  if (rows.empty()) return false;
  return apply([&](auto& qq) { return qq.remove(rows); });
}

bool QueueTableModel::toggle_skip(std::vector<std::size_t> rows) {
  if (rows.empty()) return false;
  return apply([&](auto& qq) { return qq.toggle_skip(rows); });
}

bool QueueTableModel::toggle_end_after(std::size_t row) {
  return apply([&](auto& qq) { return qq.toggle_end_after(row); });
}

void QueueTableModel::set_locked(bool locked) {
  if (locked_ == locked) return;
  locked_ = locked;
  if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, Count - 1));
}

void QueueTableModel::clear_status() {
  status_.clear();
  run_rows_.clear();
  if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, Count - 1));
}

void QueueTableModel::row_changed(int row) {
  if (row >= 0 && row < rowCount()) emit dataChanged(index(row, 0), index(row, Count - 1));
}

void QueueTableModel::on_run_started(const experiment::executor::RunStarted& e) {
  const int row = static_cast<int>(e.row);
  status_[row] = RowStatus{e.run_id, RunState::Preparing, false, false, 0, {}, {}};
  run_rows_[e.run_id] = row;
  row_changed(row);
}

void QueueTableModel::on_run_state(const experiment::run::RunStateChanged& e) {
  auto it = run_rows_.find(e.run_id);
  if (it == run_rows_.end()) return;
  RowStatus& st = status_[it->second];
  if (st.finished) return;
  st.state = e.to;
  if (e.to == RunState::Truncated) st.truncated = true;
  row_changed(it->second);
}

void QueueTableModel::on_run_finished(const experiment::executor::RunFinished& e) {
  const auto& s = e.summary;
  const int row = static_cast<int>(s.row);
  RowStatus& st = status_[row];
  st.run_id = s.run_id;
  st.state = s.state;
  st.finished = true;
  st.truncated = s.truncated;
  st.aliquot = s.aliquot;
  st.step = s.step;
  st.error = s.error ? q(*s.error) : QString();
  if (s.save_error && st.error.isEmpty()) st.error = tr("saved to the spool only");
  run_rows_[s.run_id] = row;
  row_changed(row);
}

std::optional<RunState> QueueTableModel::status(int row) const {
  auto it = status_.find(row);
  if (it == status_.end()) return std::nullopt;
  return it->second.state;
}

QString QueueTableModel::status_text(int row) const {
  auto it = status_.find(row);
  if (it == status_.end()) return {};
  QString t = q(experiment::run::to_string(it->second.state));
  if (it->second.finished && it->second.truncated) t += tr(" (truncated)");
  return t;
}

QColor QueueTableModel::state_color(RunState state) {
  switch (state) {
    case RunState::Success: return {0xc8, 0xe6, 0xc9};
    case RunState::Failed: return {0xf8, 0xd7, 0xda};
    case RunState::Cancelled:
    case RunState::Aborted: return {0xff, 0xe0, 0xb2};
    default: return {0xbb, 0xde, 0xfb};  // in progress
  }
}

}  // namespace pychron::ui
