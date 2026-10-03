#include "analysis_table_model.hpp"

#include <cmath>

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QTimeZone>

namespace pychron::ui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// Row tint by analysis type (legacy use_analysis_colors).
QColor type_color(const std::string& t) {
  if (t == "unknown") return {};
  if (t.rfind("blank", 0) == 0) return QColor(230, 240, 255);
  if (t == "air") return QColor(235, 250, 235);
  if (t == "cocktail") return QColor(255, 245, 225);
  if (t == "detector_ic") return QColor(245, 235, 255);
  return {};
}

}  // namespace

AnalysisTableModel::AnalysisTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void AnalysisTableModel::set_rows(std::vector<processing::AnalysisSummary> rows) {
  beginResetModel();
  rows_ = std::move(rows);
  endResetModel();
}

void AnalysisTableModel::append_rows(std::vector<processing::AnalysisSummary> rows) {
  if (rows.empty()) return;
  const int first = static_cast<int>(rows_.size());
  beginInsertRows({}, first, first + static_cast<int>(rows.size()) - 1);
  for (auto& r : rows) rows_.push_back(std::move(r));
  endInsertRows();
}

int AnalysisTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int AnalysisTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QString AnalysisTableModel::format_time(double t) {
  return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(std::llround(t)), QTimeZone::utc()).toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
}

QVariant AnalysisTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= static_cast<int>(rows_.size())) return {};
  const auto& r = rows_[static_cast<std::size_t>(index.row())];
  if (role == Qt::BackgroundRole) {
    if (r.tag != "ok" && !r.tag.empty()) return QBrush(QColor(255, 228, 225));
    const QColor c = type_color(r.analysis_type);
    return c.isValid() ? QVariant(QBrush(c)) : QVariant();
  }
  if (role == Qt::UserRole) return qs(r.uuid);
  if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
  switch (index.column()) {
    case RunId:
      return qs(r.runid);
    case Type:
      return qs(r.analysis_type);
    case Sample:
      return qs(r.sample);
    case Identifier:
      return qs(r.identifier);
    case Spectrometer:
      return qs(r.mass_spectrometer);
    case Date:
      return format_time(r.timestamp);
    case Extract:
      if (!r.extract_value) return {};
      return QString::number(*r.extract_value, 'g', 6) + (r.extract_units.empty() ? QString() : QStringLiteral(" ") + qs(r.extract_units));
    case Tag:
      return qs(r.tag);
    case Project:
      return qs(r.project);
    case Irradiation:
      return qs(r.irradiation + (r.level.empty() ? "" : " " + r.level));
    case Uuid:
      return qs(r.uuid);
    default:
      return {};
  }
}

QVariant AnalysisTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* names[] = {"Run ID", "Type", "Sample", "Identifier", "Spec.", "Date (UTC)", "Extract", "Tag",
                                "Project", "Irradiation", "UUID"};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

}  // namespace pychron::ui
