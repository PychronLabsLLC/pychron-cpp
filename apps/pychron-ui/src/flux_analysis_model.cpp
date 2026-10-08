#include "flux_analysis_model.hpp"

#include "pychron/processing/flux_view.hpp"

namespace pychron::ui {

namespace {

namespace pp = pychron::processing;

QString j_text(const std::optional<double>& v) {
  const std::string s = pp::flux_j_text(v);
  return s == "-" ? QString() : QString::fromStdString(s);
}

bool checkable(pp::AnalysisState s) { return s != pp::AnalysisState::NotReduced && s != pp::AnalysisState::NoJ; }

}  // namespace

FluxAnalysisModel::FluxAnalysisModel(QObject* parent) : QAbstractTableModel(parent) {}

void FluxAnalysisModel::set_position(const processing::FittedPosition* position) {
  beginResetModel();
  position_ = position;
  endResetModel();
}

const processing::FittedPosition::UsedAnalysis* FluxAnalysisModel::at(int row) const {
  if (!position_ || row < 0 || row >= static_cast<int>(position_->analyses.size())) return nullptr;
  return &position_->analyses[static_cast<std::size_t>(row)];
}

int FluxAnalysisModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() || !position_ ? 0 : static_cast<int>(position_->analyses.size());
}

int FluxAnalysisModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant FluxAnalysisModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* const names[] = {"Use", "Record", "Tag", "J", "±", "State"};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

QVariant FluxAnalysisModel::data(const QModelIndex& index, int role) const {
  const auto* a = index.isValid() ? at(index.row()) : nullptr;
  if (!a) return {};
  const int col = index.column();
  if (role == Qt::CheckStateRole) {
    if (col == Use) return a->state == pp::AnalysisState::Used ? Qt::Checked : Qt::Unchecked;
    return {};
  }
  if (role == Qt::TextAlignmentRole && (col == J || col == JErr)) return int(Qt::AlignRight | Qt::AlignVCenter);
  if (role == Qt::ToolTipRole) {
    if (col == Use && a->state == pp::AnalysisState::NotReduced)
      return a->reduction_error.empty() ? tr("Not reduced") : QString::fromStdString(a->reduction_error);
    if (col == Use && a->state == pp::AnalysisState::NoJ) return tr("No J");
    return {};
  }
  if (role != Qt::DisplayRole) return {};
  switch (col) {
    case Record: return QString::fromStdString(a->record_id);
    case Tag: return QString::fromStdString(a->tag);
    case J: return j_text(a->j);
    case JErr: return j_text(a->j_err);
    case State:
      switch (a->state) {
        case pp::AnalysisState::Used: return tr("used");
        case pp::AnalysisState::OmittedByTag: return tr("omitted (tag %1)").arg(QString::fromStdString(a->tag));
        case pp::AnalysisState::OmittedBySavedFit: return tr("omitted (saved fit)");
        case pp::AnalysisState::OmittedByEdit: return tr("omitted (here)");
        case pp::AnalysisState::NotReduced: return tr("not reduced");
        case pp::AnalysisState::NoJ: return tr("no J");
      }
      return {};
    default: return {};
  }
}

Qt::ItemFlags FluxAnalysisModel::flags(const QModelIndex& index) const {
  Qt::ItemFlags f = QAbstractTableModel::flags(index);
  const auto* a = index.isValid() ? at(index.row()) : nullptr;
  if (a && index.column() == Use && checkable(a->state)) f |= Qt::ItemIsUserCheckable;
  return f;
}

bool FluxAnalysisModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (!index.isValid() || role != Qt::CheckStateRole || !(flags(index) & Qt::ItemIsUserCheckable)) return false;
  Q_EMIT use_toggled(QString::fromStdString(at(index.row())->uuid), value.toInt() == Qt::Checked);
  return true;
}

}  // namespace pychron::ui
