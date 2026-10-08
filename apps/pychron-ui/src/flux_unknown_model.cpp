#include "flux_unknown_model.hpp"

#include "pychron/processing/flux_view.hpp"

namespace pychron::ui {

namespace {

namespace pp = pychron::processing;

}  // namespace

FluxUnknownModel::FluxUnknownModel(QObject* parent) : FluxPositionModel(false, parent) {}

int FluxUnknownModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant FluxUnknownModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* const names[] = {QT_TR_NOOP("Save"), QT_TR_NOOP("Hole"), QT_TR_NOOP("Identifier"), QT_TR_NOOP("Sample"), QT_TR_NOOP("Saved J"), QT_TR_NOOP("±"), QT_TR_NOOP("Pred. J"), QT_TR_NOOP("±"), QT_TR_NOOP("%"), QT_TR_NOOP("Dev %")};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

QVariant FluxUnknownModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0) return {};
  const pp::FittedPosition* f = fitted(index.row());
  const pp::LevelPosition* in = input(index.row());
  if (!f && !in) return {};
  const int hole = f ? f->hole : in->hole;
  const int col = index.column();

  if (role == Qt::CheckStateRole) {
    if (col == Save) return saved(hole) ? Qt::Checked : Qt::Unchecked;
    return {};
  }
  if (role == Qt::TextAlignmentRole && col != Save && col != Identifier && col != Sample)
    return int(Qt::AlignRight | Qt::AlignVCenter);
  if (role != Qt::DisplayRole) return {};

  switch (col) {
    case Hole: return hole;
    case Identifier: return QString::fromStdString(f ? f->identifier : in->identifier);
    case Sample: return QString::fromStdString(f ? f->sample : in->sample);
    case SavedJ: return flux_j_cell(f ? f->saved_j : (in->saved ? in->saved->j : std::nullopt));
    case SavedJErr: return flux_j_cell(f ? f->saved_j_err : (in->saved ? in->saved->j_err : std::nullopt));
    case PredJ: return f ? flux_j_cell(f->j) : QString();
    case PredJErr: return f ? flux_j_cell(f->j_err) : QString();
    case PredPercent: return f ? flux_percent_cell(f->j_err, f->j) : QString();
    case Dev: return f ? flux_fixed2_cell(f->dev_percent) : QString();
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
