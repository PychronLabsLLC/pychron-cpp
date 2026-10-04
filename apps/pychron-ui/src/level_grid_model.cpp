#include "level_grid_model.hpp"

#include <QBrush>

#include "theme.hpp"

namespace pychron::ui {

namespace {

QString opt(const std::optional<std::string>& v) { return v ? QString::fromStdString(*v) : QString(); }

bool changed(const entry::SheetRow& r) {
  if (!r.stored) return !r.empty();
  const auto& s = *r.stored;
  return r.position != s.position || r.sample != s.sample || r.weight != s.weight || r.packet != s.packet ||
         r.note != s.note;
}

}  // namespace

LevelGridModel::LevelGridModel(QObject* parent) : QAbstractTableModel(parent) {}

void LevelGridModel::set_edit(std::optional<entry::LevelSheetEdit> edit) {
  beginResetModel();
  edit_ = std::move(edit);
  endResetModel();
}

int LevelGridModel::position_at(int row) const {
  if (!edit_ || row < 0 || row >= rowCount()) return 0;
  return edit_->rows()[static_cast<std::size_t>(row)].position;
}

int LevelGridModel::row_of(int position) const {
  if (!edit_) return -1;
  for (std::size_t i = 0; i < edit_->rows().size(); ++i)
    if (edit_->rows()[i].position == position) return static_cast<int>(i);
  return -1;
}

int LevelGridModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() || !edit_ ? 0 : static_cast<int>(edit_->rows().size());
}

int LevelGridModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant LevelGridModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* const names[] = {"", "Hole", "Packet", "Identifier", "Sample", "Project", "PI",
                                      "Material", "Grainsize", "Weight", "J", "±J", "Note"};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

QVariant LevelGridModel::data(const QModelIndex& index, int role) const {
  if (!edit_ || !index.isValid()) return {};
  const entry::SheetRow& r = edit_->rows()[static_cast<std::size_t>(index.row())];
  if (role == Qt::DisplayRole || role == Qt::EditRole) {
    switch (index.column()) {
      case Analyzed: return r.analyzed() ? QStringLiteral("X") : QString();
      case Position:
        return r.hole_id && *r.hole_id != std::to_string(r.position)
                   ? QStringLiteral("%1 (%2)").arg(r.position).arg(QString::fromStdString(*r.hole_id))
                   : QString::number(r.position);
      case Packet: return opt(r.packet);
      case Identifier: return r.stored ? opt(r.stored->identifier) : QString();
      case Sample: return QString::fromStdString(r.sample_name);
      case Project: return QString::fromStdString(r.project);
      case PI: return QString::fromStdString(r.principal_investigator);
      case Material: return QString::fromStdString(r.material);
      case Grainsize: return QString::fromStdString(r.grainsize);
      case Weight: return r.weight ? QString::number(*r.weight, 'g', 8) : QString();
      case J: return r.stored && r.stored->j ? QString::number(*r.stored->j, 'E', 6) : QString();
      case JErr: return r.stored && r.stored->j_err ? QString::number(*r.stored->j_err, 'E', 6) : QString();
      case Note: return opt(r.note);
      default: return {};
    }
  }
  if (role == Qt::ToolTipRole && index.column() == Analyzed && r.stored && r.analyzed())
    return tr("%1 analyses%2").arg(r.stored->n_analyses).arg(r.stored->in_load ? tr(", in a load") : QString());
  if (role == Qt::BackgroundRole) {
    if (r.orphan) return QBrush(theme().warning_bg);
    if (changed(r)) return QBrush(theme().diff_changed);
    if (r.analyzed()) return QBrush(theme().row_air);
  }
  return {};
}

Qt::ItemFlags LevelGridModel::flags(const QModelIndex& index) const {
  Qt::ItemFlags f = QAbstractTableModel::flags(index);
  if (index.column() == Weight || index.column() == Packet || index.column() == Note) f |= Qt::ItemIsEditable;
  return f;
}

bool LevelGridModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (!edit_ || role != Qt::EditRole || !index.isValid()) return false;
  const int position = position_at(index.row());
  const QString t = value.toString().trimmed();
  switch (index.column()) {
    case Weight: {
      std::optional<double> w;
      if (!t.isEmpty()) {
        bool ok = false;
        w = t.toDouble(&ok);
        if (!ok) return false;
      }
      edit_->set_weight(position, w);
      break;
    }
    case Packet:
      if (!t.isEmpty() && !entry::valid_packet(t.toStdString())) return false;
      edit_->set_packet(position, t.isEmpty() ? std::nullopt : std::optional<std::string>(t.toStdString()));
      break;
    case Note:
      edit_->set_note(position, t.isEmpty() ? std::nullopt : std::optional<std::string>(t.toStdString()));
      break;
    default:
      return false;
  }
  Q_EMIT dataChanged(this->index(index.row(), 0), this->index(index.row(), ColumnCount - 1));
  Q_EMIT edited();
  return true;
}

}  // namespace pychron::ui
