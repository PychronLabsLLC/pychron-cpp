#include "flux_monitor_model.hpp"

#include <algorithm>

#include <QBrush>

#include "pychron/processing/flux_view.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

namespace pp = pychron::processing;

bool has_note(const pp::FittedPosition& p, pp::PositionNote note) {
  return std::find(p.notes.begin(), p.notes.end(), note) != p.notes.end();
}

bool is_numeric(int column) {
  return column != FluxMonitorModel::Fit && column != FluxMonitorModel::Save && column != FluxMonitorModel::Identifier &&
         column != FluxMonitorModel::Sample;
}

}  // namespace

FluxMonitorModel::FluxMonitorModel(QObject* parent) : FluxPositionModel(true, parent) {}

int FluxMonitorModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant FluxMonitorModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* const names[] = {QT_TR_NOOP("Fit"), QT_TR_NOOP("Save"), QT_TR_NOOP("Hole"), QT_TR_NOOP("Identifier"), QT_TR_NOOP("Sample"), QT_TR_NOOP("N"), QT_TR_NOOP("Saved J"), QT_TR_NOOP("±"),
                                      QT_TR_NOOP("Mean J"), QT_TR_NOOP("±"), QT_TR_NOOP("%"), QT_TR_NOOP("MSWD"), QT_TR_NOOP("Pred. J"), QT_TR_NOOP("±"), QT_TR_NOOP("%"), QT_TR_NOOP("Dev %")};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

QVariant FluxMonitorModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0) return {};
  const pp::FittedPosition* f = fitted(index.row());
  const pp::LevelPosition* in = input(index.row());
  if (!f && !in) return {};
  const int hole = f ? f->hole : in->hole;
  const int col = index.column();

  if (role == Qt::CheckStateRole) {
    if (col == Fit) return f && f->used_in_fit ? Qt::Checked : Qt::Unchecked;
    if (col == Save) return saved(hole) ? Qt::Checked : Qt::Unchecked;
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
    case SavedJ: return flux_j_cell(f ? f->saved_j : (in->saved ? in->saved->j : std::nullopt));
    case SavedJErr: return flux_j_cell(f ? f->saved_j_err : (in->saved ? in->saved->j_err : std::nullopt));
    case MeanJ: return f ? flux_j_cell(f->mean_j) : QString();
    case MeanJErr: return f ? flux_j_cell(f->mean_j_err) : QString();
    case MeanPercent: return f ? flux_percent_cell(f->mean_j_err, f->mean_j) : QString();
    case Mswd: return f ? flux_fixed2_cell(f->mean_j_mswd) : QString();
    case PredJ: return f ? flux_j_cell(f->j) : QString();
    case PredJErr: return f ? flux_j_cell(f->j_err) : QString();
    case PredPercent: return f ? flux_percent_cell(f->j_err, f->j) : QString();
    case Dev: return f ? flux_fixed2_cell(f->dev_percent) : QString();
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
