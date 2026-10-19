#include "field_table_model.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QUndoCommand>

namespace pychron::ui {

namespace sp = pychron::spectrometer;

namespace {

QString number(double v) { return QString::number(v, 'f', 5); }

// Finite double from typed text; nullopt for anything else.
std::optional<double> parse_finite(const QString& text) {
  bool ok = false;
  const double v = text.trimmed().toDouble(&ok);
  if (!ok || !std::isfinite(v)) return std::nullopt;
  return v;
}

bool same_mass(double a, double b) { return std::abs(a - b) < 1e-9; }

}  // namespace

void sort_by_mass(sp::FieldTable& table) {
  std::ranges::stable_sort(table.mutable_points(), {}, &sp::ControlPoint::mass);
}

class FieldTableModel::Edit : public QUndoCommand {
 public:
  Edit(FieldTableModel& model, sp::FieldTable before, sp::FieldTable after, const QString& text)
      : QUndoCommand(text), model_(model), before_(std::move(before)), after_(std::move(after)) {}

  void undo() override { model_.apply(before_); }
  void redo() override { model_.apply(after_); }

 private:
  FieldTableModel& model_;
  sp::FieldTable before_;
  sp::FieldTable after_;
};

FieldTableModel::FieldTableModel(std::vector<std::string> config_detectors, sp::MolecularWeights weights,
                                 QObject* parent)
    : QAbstractTableModel(parent), config_detectors_(std::move(config_detectors)), weights_(std::move(weights)) {
  columns_ = config_detectors_;
}

void FieldTableModel::set_table(sp::FieldTable table) {
  sort_by_mass(table);
  beginResetModel();
  table_ = std::move(table);
  columns_ = config_detectors_;
  for (const auto& det : table_.detectors()) {
    if (std::ranges::find(columns_, det) == columns_.end()) columns_.push_back(det);
  }
  undo_.clear();
  undo_.setClean();
  endResetModel();
  emit tableEdited();
}

void FieldTableModel::set_read_only(bool on) {
  if (read_only_ == on) return;
  read_only_ = on;
  if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

bool FieldTableModel::in_config(const std::string& detector) const {
  return std::ranges::find(config_detectors_, detector) != config_detectors_.end();
}

int FieldTableModel::column_of(const std::string& detector) const {
  const auto it = std::ranges::find(columns_, detector);
  if (it == columns_.end()) return -1;
  return kFirstDetectorColumn + static_cast<int>(it - columns_.begin());
}

std::string FieldTableModel::detector_at(int column) const {
  const int i = column - kFirstDetectorColumn;
  if (i < 0 || std::cmp_greater_equal(i, columns_.size())) return {};
  return columns_[static_cast<std::size_t>(i)];
}

void FieldTableModel::push(sp::FieldTable after, const QString& text) {
  sort_by_mass(after);
  if (after == table_) return;
  undo_.push(new Edit(*this, table_, std::move(after), text));  // the stack owns it
}

void FieldTableModel::apply(sp::FieldTable table) {
  const auto& old_points = table_.points();
  const auto& new_points = table.points();
  const bool same_rows = old_points.size() == new_points.size() &&
                         std::ranges::equal(old_points, new_points, {}, &sp::ControlPoint::isotope,
                                            &sp::ControlPoint::isotope);
  if (same_rows) {
    table_ = std::move(table);
    if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
  } else {
    beginResetModel();
    table_ = std::move(table);
    endResetModel();
  }
  emit tableEdited();
}

void FieldTableModel::edit_table(sp::FieldTable after, const QString& text) {
  if (read_only_) return;
  push(std::move(after), text);
}

void FieldTableModel::add_row(const QString& isotope) {
  if (read_only_) return;
  const std::string name = isotope.trimmed().toStdString();
  const auto mass = weights_.mass(name);
  sp::FieldTable after = table_;
  after.mutable_points().push_back(sp::ControlPoint{name, mass ? *mass : 0.0, {}});
  push(std::move(after), tr("Add Row"));
}

void FieldTableModel::remove_rows(std::vector<int> rows) {
  if (read_only_) return;
  std::ranges::sort(rows, std::greater<>());
  const auto [first, last] = std::ranges::unique(rows);
  rows.erase(first, last);
  sp::FieldTable after = table_;
  auto& points = after.mutable_points();
  for (const int r : rows) {
    if (r >= 0 && std::cmp_less(r, points.size())) points.erase(points.begin() + r);
  }
  push(std::move(after), tr("Remove Row"));
}

Result<void> FieldTableModel::set_value(const std::string& det, const std::string& isotope, double value,
                                        bool propagate) {
  if (read_only_) return fail(ErrorKind::Config, "this version is read-only");
  if (!std::isfinite(value)) return fail(ErrorKind::Config, "value is not finite");
  sp::FieldTable after = table_;
  if (auto r = after.update(det, isotope, value, propagate); !r) return r;
  push(std::move(after), tr("Set Value"));
  return {};
}

void FieldTableModel::set_default_fit(sp::FitKind kind) {
  if (read_only_) return;
  sp::FieldTable after = table_;
  after.set_default_fit(kind);
  push(std::move(after), tr("Default Fit"));
}

void FieldTableModel::set_detector_fit(const std::string& det, std::optional<sp::FitKind> kind) {
  if (read_only_) return;
  sp::FieldTable after = table_;
  if (kind) {
    after.set_fit(det, *kind);
  } else {
    after.clear_fit(det);
  }
  push(std::move(after), tr("Detector Fit"));
}

int FieldTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(table_.points().size());
}

int FieldTableModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : kFirstDetectorColumn + static_cast<int>(columns_.size());
}

QVariant FieldTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= rowCount()) return {};
  const auto& point = table_.points()[static_cast<std::size_t>(index.row())];
  const int col = index.column();
  if (role == Qt::TextAlignmentRole && col != kIsotopeColumn) {
    return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
  }
  if (col == kIsotopeColumn) {
    if (role == Qt::DisplayRole || role == Qt::EditRole) return QString::fromStdString(point.isotope);
    return {};
  }
  if (col == kMassColumn) {
    if (role == Qt::DisplayRole) return number(point.mass);
    if (role == Qt::EditRole) return QString::number(point.mass, 'g', 17);
    return {};
  }
  const std::string det = detector_at(col);
  if (det.empty()) return {};
  const auto it = point.values.find(det);
  if (it == point.values.end()) return {};
  if (role == Qt::DisplayRole) return number(it->second);
  if (role == Qt::EditRole) return QString::number(it->second, 'g', 17);
  if (role == Qt::ToolTipRole) {
    if (table_.fit(det) == sp::FitKind::Discrete || !table_.check_fit(det)) return {};
    const auto fit = table_.value_for(point.mass, det);
    if (!fit) return {};
    return tr("residual %1").arg(number(it->second - *fit));
  }
  return {};
}

bool FieldTableModel::set_isotope(std::size_t row, const QString& text) {
  const std::string name = text.trimmed().toStdString();
  sp::FieldTable after = table_;
  auto& point = after.mutable_points()[row];
  if (point.isotope == name) return true;
  const auto old_weight = weights_.mass(point.isotope);
  const bool follows = point.mass == 0.0 || (old_weight && same_mass(point.mass, *old_weight));
  if (const auto weight = weights_.mass(name); weight && follows) point.mass = *weight;
  point.isotope = name;
  push(std::move(after), tr("Edit Isotope"));
  return true;
}

bool FieldTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (read_only_ || role != Qt::EditRole || !index.isValid() || index.row() >= rowCount()) return false;
  const auto row = static_cast<std::size_t>(index.row());
  const QString text = value.toString();
  const int col = index.column();
  if (col == kIsotopeColumn) return set_isotope(row, text);
  if (col == kMassColumn) {
    const auto mass = parse_finite(text);
    if (!mass || *mass <= 0) return false;
    sp::FieldTable after = table_;
    after.mutable_points()[row].mass = *mass;
    push(std::move(after), tr("Edit Mass"));
    return true;
  }
  const std::string det = detector_at(col);
  if (det.empty()) return false;
  sp::FieldTable after = table_;
  auto& values = after.mutable_points()[row].values;
  if (text.trimmed().isEmpty()) {
    values.erase(det);
  } else {
    const auto v = parse_finite(text);
    if (!v) return false;
    values[det] = *v;
  }
  push(std::move(after), tr("Edit Value"));
  return true;
}

Qt::ItemFlags FieldTableModel::flags(const QModelIndex& index) const {
  if (!index.isValid()) return Qt::NoItemFlags;
  Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  if (!read_only_) f |= Qt::ItemIsEditable;
  return f;
}

QVariant FieldTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal) return QAbstractTableModel::headerData(section, orientation, role);
  if (section == kIsotopeColumn) return role == Qt::DisplayRole ? QVariant(tr("Isotope")) : QVariant();
  if (section == kMassColumn) return role == Qt::DisplayRole ? QVariant(tr("Mass")) : QVariant();
  const std::string det = detector_at(section);
  if (det.empty()) return {};
  const QString name = QString::fromStdString(det);
  if (role == Qt::DisplayRole) return in_config(det) ? name : tr("%1 (not in config)").arg(name);
  if (role == Qt::ToolTipRole && !in_config(det)) return tr("%1 is not in config; its values are kept").arg(name);
  return {};
}

}  // namespace pychron::ui
