#include "conditional_table_model.hpp"

#include <algorithm>
#include <map>

#include "theme.hpp"

namespace pychron::ui {

using experiment::Conditional;
using experiment::ConditionalKind;
using experiment::ConditionalSet;

namespace {

int rank(ConditionalKind kind) {
  int i = 0;
  for (const ConditionalKind k : experiment::kFileOrder) {
    if (k == kind) return i;
    ++i;
  }
  return i;
}

std::string name_of(const Conditional& c) {
  return c.name.empty() ? experiment::default_name(c.kind, c.check) : c.name;
}

bool same_sets(const ConditionalSet& a, const ConditionalSet& b) {
  return a.disable == b.disable &&
         std::equal(a.items.begin(), a.items.end(), b.items.begin(), b.items.end(), same_authored);
}

}  // namespace

bool same_authored(const Conditional& a, const Conditional& b) {
  return a.name == b.name && a.kind == b.kind && a.check == b.check && a.start == b.start &&
         a.frequency == b.frequency && a.ntrips == b.ntrips && a.window == b.window && a.mapper == b.mapper &&
         a.analysis_types == b.analysis_types && a.abbreviated_count_ratio == b.abbreviated_count_ratio &&
         a.action == b.action && a.resume == b.resume && a.truncate == b.truncate && a.terminate == b.terminate;
}

ConditionalTableModel::ConditionalTableModel(QObject* parent) : QAbstractTableModel(parent) {}

int ConditionalTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(set_.items.size());
}

int ConditionalTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : Count; }

QVariant ConditionalTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  switch (section) {
    case Kind: return tr("Kind");
    case Name: return tr("Name");
    case Check: return tr("Check");
    case Action: return tr("Action");
    default: return {};
  }
}

QVariant ConditionalTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || !valid(index.row())) return {};
  const Conditional& c = set_.items[static_cast<std::size_t>(index.row())];
  const QString& err = errors_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case Qt::DisplayRole:
      switch (index.column()) {
        case Kind: return QString::fromUtf8(to_string(c.kind));
        case Name: return QString::fromStdString(name_of(c));
        case Check: return QString::fromStdString(c.check);
        case Action:
          if (experiment::fields_of(c.kind).actions.empty()) return QStringLiteral("-");
          return QString::fromStdString(to_string(c.action));
        default: return {};
      }
    case Qt::ForegroundRole:
      if (!err.isEmpty()) return theme().error_text;
      if (index.column() == Name && c.name.empty()) return theme().muted_text;
      return {};
    case Qt::ToolTipRole: return err.isEmpty() ? QVariant() : QVariant(err);
    default: return {};
  }
}

void ConditionalTableModel::set(ConditionalSet set) {
  beginResetModel();
  std::stable_sort(set.items.begin(), set.items.end(),
                   [](const Conditional& a, const Conditional& b) { return rank(a.kind) < rank(b.kind); });
  for (auto& c : set.items) {
    // The parser fills in the default name; keep it a default so it follows the check.
    if (c.name == experiment::default_name(c.kind, c.check)) c.name.clear();
    c.expr.reset();
  }
  set_ = std::move(set);
  clean_ = set_;
  recheck();
  endResetModel();
}

void ConditionalTableModel::mark_clean() { clean_ = set_; }

bool ConditionalTableModel::modified() const { return !same_sets(set_, clean_); }

void ConditionalTableModel::recheck() {
  errors_.assign(set_.items.size(), QString());
  std::map<std::string, int> seen;
  for (std::size_t i = 0; i < set_.items.size(); ++i) {
    const Conditional& c = set_.items[i];
    if (auto f = experiment::finalize(c); !f) {
      errors_[i] = QString::fromStdString(f.error().what);
      continue;
    }
    const std::string name = name_of(c);
    if (!seen.emplace(name, static_cast<int>(i)).second)
      errors_[i] = tr("duplicate name '%1'").arg(QString::fromStdString(name));
  }
}

void ConditionalTableModel::touched() {
  recheck();
  if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, Count - 1));
  emit changed();
}

int ConditionalTableModel::add(ConditionalKind kind) {
  Conditional c;
  c.kind = kind;
  c.action.type = experiment::fields_of(kind).default_action;
  const auto at = std::upper_bound(set_.items.begin(), set_.items.end(), rank(kind),
                                   [](int r, const Conditional& x) { return r < rank(x.kind); });
  const int row = static_cast<int>(at - set_.items.begin());
  beginInsertRows({}, row, row);
  set_.items.insert(at, std::move(c));
  errors_.insert(errors_.begin() + row, QString());
  endInsertRows();
  touched();
  return row;
}

bool ConditionalTableModel::remove(int row) {
  if (!valid(row)) return false;
  beginRemoveRows({}, row, row);
  set_.items.erase(set_.items.begin() + row);
  errors_.erase(errors_.begin() + row);
  endRemoveRows();
  touched();
  return true;
}

int ConditionalTableModel::duplicate(int row) {
  if (!valid(row)) return -1;
  Conditional c = set_.items[static_cast<std::size_t>(row)];
  if (!c.name.empty()) c.name += "_copy";
  beginInsertRows({}, row + 1, row + 1);
  set_.items.insert(set_.items.begin() + row + 1, std::move(c));
  errors_.insert(errors_.begin() + row + 1, QString());
  endInsertRows();
  touched();
  return row + 1;
}

bool ConditionalTableModel::move_up(int row) { return valid(row) && move_down(row - 1); }

bool ConditionalTableModel::move_down(int row) {
  if (!valid(row) || !valid(row + 1)) return false;
  auto& items = set_.items;
  const auto i = static_cast<std::size_t>(row);
  if (items[i].kind != items[i + 1].kind) return false;
  std::swap(items[i], items[i + 1]);
  touched();
  return true;
}

int ConditionalTableModel::replace(int row, Conditional c) {
  if (!valid(row)) return -1;
  c.expr.reset();
  auto& items = set_.items;
  const auto i = static_cast<std::size_t>(row);
  if (c.kind == items[i].kind) {
    items[i] = std::move(c);
    touched();
    return row;
  }
  beginResetModel();
  const ConditionalKind kind = c.kind;
  items.erase(items.begin() + row);
  const auto at = std::upper_bound(items.begin(), items.end(), rank(kind),
                                   [](int r, const Conditional& x) { return r < rank(x.kind); });
  const int moved = static_cast<int>(at - items.begin());
  items.insert(at, std::move(c));
  recheck();
  endResetModel();
  emit changed();
  return moved;
}

QString ConditionalTableModel::effective_name(int row) const {
  return valid(row) ? QString::fromStdString(name_of(set_.items[static_cast<std::size_t>(row)])) : QString();
}

int ConditionalTableModel::row_of(const QString& effective_name) const {
  const std::string name = effective_name.toStdString();
  for (std::size_t i = 0; i < set_.items.size(); ++i)
    if (name_of(set_.items[i]) == name) return static_cast<int>(i);
  return -1;
}

void ConditionalTableModel::set_disable(const QStringList& names) {
  set_.disable.clear();
  for (const QString& n : names) set_.disable.push_back(n.toStdString());
  emit changed();
}

QStringList ConditionalTableModel::disable() const {
  QStringList out;
  for (const auto& n : set_.disable) out.append(QString::fromStdString(n));
  return out;
}

QString ConditionalTableModel::error(int row) const {
  return valid(row) ? errors_[static_cast<std::size_t>(row)] : QString();
}

bool ConditionalTableModel::has_errors() const {
  return std::any_of(errors_.begin(), errors_.end(), [](const QString& e) { return !e.isEmpty(); });
}

}  // namespace pychron::ui
