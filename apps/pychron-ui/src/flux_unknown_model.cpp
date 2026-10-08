#include "flux_unknown_model.hpp"

#include "pychron/processing/flux_view.hpp"

namespace pychron::ui {

namespace {

namespace pp = pychron::processing;

QString text(const std::string& s) { return s == "-" ? QString() : QString::fromStdString(s); }
QString j_text(const std::optional<double>& v) { return text(pp::flux_j_text(v)); }

}  // namespace

FluxUnknownModel::FluxUnknownModel(QObject* parent) : QAbstractTableModel(parent) {}

void FluxUnknownModel::rebuild() {
  rows_.clear();
  if (fit_) {
    for (std::size_t i = 0; i < fit_->positions.size(); ++i)
      if (!fit_->positions[i].monitor) rows_.push_back(i);
  } else if (inputs_) {
    for (std::size_t i = 0; i < inputs_->positions.size(); ++i)
      if (!inputs_->positions[i].monitor) rows_.push_back(i);
  }
}

void FluxUnknownModel::set_fit(const processing::LevelFit* fit) {
  beginResetModel();
  fit_ = fit;
  rebuild();
  endResetModel();
}

void FluxUnknownModel::set_inputs(const processing::LevelInputs* inputs) {
  beginResetModel();
  inputs_ = inputs;
  rebuild();
  endResetModel();
}

void FluxUnknownModel::set_skip(const std::set<int>& skip_positions) {
  beginResetModel();
  skip_ = skip_positions;
  endResetModel();
}

const processing::FittedPosition* FluxUnknownModel::fitted(int row) const {
  if (!fit_ || row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
  return &fit_->positions[rows_[static_cast<std::size_t>(row)]];
}

const processing::LevelPosition* FluxUnknownModel::input(int row) const {
  if (fit_ || !inputs_ || row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
  return &inputs_->positions[rows_[static_cast<std::size_t>(row)]];
}

int FluxUnknownModel::hole_at(int row) const {
  if (const auto* f = fitted(row)) return f->hole;
  if (const auto* i = input(row)) return i->hole;
  return 0;
}

int FluxUnknownModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int FluxUnknownModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant FluxUnknownModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* const names[] = {"Save", "Hole", "Identifier", "Sample", "Saved J", "±", "Pred. J", "±", "%", "Dev %"};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

QVariant FluxUnknownModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) return {};
  const pp::FittedPosition* f = fitted(index.row());
  const pp::LevelPosition* in = input(index.row());
  if (!f && !in) return {};
  const int hole = f ? f->hole : in->hole;
  const int col = index.column();

  if (role == Qt::CheckStateRole) {
    if (col == Save) return skip_.count(hole) ? Qt::Unchecked : Qt::Checked;
    return {};
  }
  if (role == Qt::TextAlignmentRole && col != Save && col != Identifier && col != Sample)
    return int(Qt::AlignRight | Qt::AlignVCenter);
  if (role != Qt::DisplayRole) return {};

  switch (col) {
    case Hole: return hole;
    case Identifier: return QString::fromStdString(f ? f->identifier : in->identifier);
    case Sample: return QString::fromStdString(f ? f->sample : in->sample);
    case SavedJ: return j_text(f ? f->saved_j : (in->saved ? in->saved->j : std::nullopt));
    case SavedJErr: return j_text(f ? f->saved_j_err : (in->saved ? in->saved->j_err : std::nullopt));
    case PredJ: return f ? j_text(f->j) : QString();
    case PredJErr: return f ? j_text(f->j_err) : QString();
    case PredPercent: return f ? text(pp::flux_percent_of(f->j_err, f->j)) : QString();
    case Dev: return f ? text(pp::flux_pct_text(f->dev_percent)) : QString();
    default: return {};
  }
}

Qt::ItemFlags FluxUnknownModel::flags(const QModelIndex& index) const {
  Qt::ItemFlags f = QAbstractTableModel::flags(index);
  if (index.isValid() && index.column() == Save) f |= Qt::ItemIsUserCheckable;
  return f;
}

bool FluxUnknownModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (!index.isValid() || role != Qt::CheckStateRole || index.column() != Save) return false;
  Q_EMIT save_toggled(hole_at(index.row()), value.toInt() == Qt::Checked);
  return true;
}

}  // namespace pychron::ui
