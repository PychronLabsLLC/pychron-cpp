#include "flux_monitor_model.hpp"

#include <algorithm>

#include <QBrush>

#include "pychron/processing/flux_view.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

namespace pp = pychron::processing;

QString text(const std::string& s) { return s == "-" ? QString() : QString::fromStdString(s); }
QString j_text(const std::optional<double>& v) { return text(pp::flux_j_text(v)); }
QString pct_text(const std::optional<double>& v) { return text(pp::flux_pct_text(v)); }

bool has_note(const pp::FittedPosition& p, pp::PositionNote note) {
  return std::find(p.notes.begin(), p.notes.end(), note) != p.notes.end();
}

bool is_numeric(int column) {
  return column != FluxMonitorModel::Fit && column != FluxMonitorModel::Save && column != FluxMonitorModel::Identifier &&
         column != FluxMonitorModel::Sample;
}

}  // namespace

FluxMonitorModel::FluxMonitorModel(QObject* parent) : QAbstractTableModel(parent) {}

void FluxMonitorModel::rebuild() {
  rows_.clear();
  if (fit_) {
    for (std::size_t i = 0; i < fit_->positions.size(); ++i)
      if (fit_->positions[i].monitor) rows_.push_back(i);
  } else if (inputs_) {
    for (std::size_t i = 0; i < inputs_->positions.size(); ++i)
      if (inputs_->positions[i].monitor) rows_.push_back(i);
  }
}

void FluxMonitorModel::set_fit(const processing::LevelFit* fit) {
  beginResetModel();
  fit_ = fit;
  rebuild();
  endResetModel();
}

void FluxMonitorModel::set_inputs(const processing::LevelInputs* inputs) {
  beginResetModel();
  inputs_ = inputs;
  rebuild();
  endResetModel();
}

void FluxMonitorModel::set_skip(const std::set<int>& skip_positions) {
  beginResetModel();
  skip_ = skip_positions;
  endResetModel();
}

const processing::FittedPosition* FluxMonitorModel::fitted(int row) const {
  if (!fit_ || row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
  return &fit_->positions[rows_[static_cast<std::size_t>(row)]];
}

const processing::LevelPosition* FluxMonitorModel::input(int row) const {
  if (fit_ || !inputs_ || row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
  return &inputs_->positions[rows_[static_cast<std::size_t>(row)]];
}

int FluxMonitorModel::hole_at(int row) const {
  if (const auto* f = fitted(row)) return f->hole;
  if (const auto* i = input(row)) return i->hole;
  return 0;
}

int FluxMonitorModel::row_of(int hole) const {
  for (int r = 0; r < static_cast<int>(rows_.size()); ++r)
    if (hole_at(r) == hole) return r;
  return -1;
}

int FluxMonitorModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int FluxMonitorModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant FluxMonitorModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* const names[] = {"Fit", "Save", "Hole", "Identifier", "Sample", "N", "Saved J", "±",
                                      "Mean J", "±", "%", "MSWD", "Pred. J", "±", "%", "Dev %"};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

QVariant FluxMonitorModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) return {};
  const pp::FittedPosition* f = fitted(index.row());
  const pp::LevelPosition* in = input(index.row());
  if (!f && !in) return {};
  const int hole = f ? f->hole : in->hole;
  const int col = index.column();

  if (role == Qt::CheckStateRole) {
    if (col == Fit) return f && f->used_in_fit ? Qt::Checked : Qt::Unchecked;
    if (col == Save) return skip_.count(hole) ? Qt::Unchecked : Qt::Checked;
    return {};
  }
  if (role == Qt::TextAlignmentRole && is_numeric(col)) return int(Qt::AlignRight | Qt::AlignVCenter);
  if (role == Qt::ToolTipRole) {
    if (col == Fit && f && has_note(*f, pp::PositionNote::NoUsableAnalysis)) return tr("No usable analysis");
    return {};
  }
  if (role == Qt::BackgroundRole) {
    if (f && has_note(*f, pp::PositionNote::MeanMswdOutsideLimits)) return QBrush(theme().warning_bg);
    return {};
  }
  if (role == Qt::ForegroundRole) {
    if (f && !f->used_in_fit) return QBrush(theme().muted_text);
    return {};
  }
  if (role != Qt::DisplayRole) return {};

  switch (col) {
    case Hole: return hole;
    case Identifier: return QString::fromStdString(f ? f->identifier : in->identifier);
    case Sample: return QString::fromStdString(f ? f->sample : in->sample);
    case N: return f ? QVariant(f->n) : QVariant(QString());
    case SavedJ: return j_text(f ? f->saved_j : (in->saved ? in->saved->j : std::nullopt));
    case SavedJErr: return j_text(f ? f->saved_j_err : (in->saved ? in->saved->j_err : std::nullopt));
    case MeanJ: return f ? j_text(f->mean_j) : QString();
    case MeanJErr: return f ? j_text(f->mean_j_err) : QString();
    case MeanPercent: return f ? text(pp::flux_percent_of(f->mean_j_err, f->mean_j)) : QString();
    case Mswd: return f ? pct_text(f->mean_j_mswd) : QString();
    case PredJ: return f ? j_text(f->j) : QString();
    case PredJErr: return f ? j_text(f->j_err) : QString();
    case PredPercent: return f ? text(pp::flux_percent_of(f->j_err, f->j)) : QString();
    case Dev: return f ? pct_text(f->dev_percent) : QString();
    default: return {};
  }
}

Qt::ItemFlags FluxMonitorModel::flags(const QModelIndex& index) const {
  Qt::ItemFlags f = QAbstractTableModel::flags(index);
  if (!index.isValid()) return f;
  if (index.column() == Save) f |= Qt::ItemIsUserCheckable;
  if (index.column() == Fit) {
    const auto* p = fitted(index.row());
    if (p && !has_note(*p, pp::PositionNote::NoUsableAnalysis)) f |= Qt::ItemIsUserCheckable;
  }
  return f;
}

bool FluxMonitorModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (!index.isValid() || role != Qt::CheckStateRole || !(flags(index) & Qt::ItemIsUserCheckable)) return false;
  const bool on = value.toInt() == Qt::Checked;
  const int hole = hole_at(index.row());
  if (index.column() == Fit) Q_EMIT fit_toggled(hole, on);
  else if (index.column() == Save) Q_EMIT save_toggled(hole, on);
  else return false;
  return true;
}

}  // namespace pychron::ui
