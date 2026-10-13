#include "analysis_table_model.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QTimeZone>

namespace pychron::ui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

AnalysisTableModel::AnalysisTableModel(QObject* parent)
    : QAbstractTableModel(parent), type_colors_(default_type_colors(theme())) {}

std::vector<AnalysisTableModel::Entry> AnalysisTableModel::entries_from(std::size_t first) {
  breaks_ = processing::time_breaks(rows_, gap_seconds_);
  std::vector<Entry> out;
  auto next = std::ranges::lower_bound(breaks_, first, {}, &processing::TimeBreak::above);
  for (std::size_t i = first; i < rows_.size(); ++i) {
    if (next != breaks_.end() && next->above == i) {
      out.push_back(Entry{true, static_cast<std::size_t>(next - breaks_.begin())});
      ++next;
    }
    out.push_back(Entry{false, i});
  }
  return out;
}

bool AnalysisTableModel::update_keys() {
  std::set<std::string> keys;
  std::set<std::string_view> spectrometers;
  for (const auto& r : rows_) {
    if (auto key = color_key(color_by_, r); !key.empty()) keys.insert(std::move(key));
    spectrometers.insert(r.mass_spectrometer);
  }
  std::vector<std::string> sorted(keys.begin(), keys.end());
  const bool several = spectrometers.size() > 1;
  const bool changed = sorted != keys_ || several != several_spectrometers_;
  keys_ = std::move(sorted);
  several_spectrometers_ = several;
  return changed;
}

void AnalysisTableModel::set_rows(std::vector<processing::AnalysisSummary> rows) {
  beginResetModel();
  rows_ = std::move(rows);
  display_ = entries_from(0);
  update_keys();
  endResetModel();
}

void AnalysisTableModel::append_rows(std::vector<processing::AnalysisSummary> rows) {
  if (rows.empty()) return;
  const std::size_t first = rows_.size();
  const int shown = static_cast<int>(display_.size());
  for (auto& r : rows) rows_.push_back(std::move(r));
  // The breaks among the rows already shown are the same and in the same
  // places (time_breaks), so their entries stand.
  const std::vector<Entry> added = entries_from(first);
  beginInsertRows({}, shown, shown + static_cast<int>(added.size()) - 1);
  display_.insert(display_.end(), added.begin(), added.end());
  endInsertRows();
  // A new key shifts the categories' colours; a second spectrometer names the separators.
  if (update_keys() && shown > 0) emit dataChanged(index(0, 0), index(shown - 1, ColumnCount - 1));
}

void AnalysisTableModel::set_coloring(ColorBy by, const TypeColors& types) {
  if (by == color_by_ && types == type_colors_) return;
  color_by_ = by;
  type_colors_ = types;
  update_keys();
  if (!display_.empty())
    emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1), {Qt::BackgroundRole});
}

void AnalysisTableModel::set_gap_threshold(double seconds) {
  if (seconds == gap_seconds_) return;
  beginResetModel();
  gap_seconds_ = seconds;
  display_ = entries_from(0);
  endResetModel();
}

bool AnalysisTableModel::is_break(int display_row) const {
  return display_row >= 0 && display_row < rowCount() && display_[static_cast<std::size_t>(display_row)].is_break;
}

const processing::AnalysisSummary* AnalysisTableModel::analysis_at(int display_row) const {
  if (display_row < 0 || display_row >= rowCount()) return nullptr;
  const Entry& e = display_[static_cast<std::size_t>(display_row)];
  return e.is_break ? nullptr : &rows_[e.index];
}

int AnalysisTableModel::display_row_of(std::size_t analysis_index) const {
  if (analysis_index >= rows_.size()) return -1;
  // The breaks above this analysis or above a newer one each push it down a row.
  const auto after = std::ranges::upper_bound(breaks_, analysis_index, {}, &processing::TimeBreak::above);
  return static_cast<int>(analysis_index) + static_cast<int>(after - breaks_.begin());
}

QList<int> AnalysisTableModel::break_rows() const {
  QList<int> out;
  for (std::size_t i = 0; i < display_.size(); ++i)
    if (display_[i].is_break) out << static_cast<int>(i);
  return out;
}

int AnalysisTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(display_.size());
}

int AnalysisTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QString AnalysisTableModel::format_time(double t) {
  return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(std::llround(t)), QTimeZone::utc()).toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
}

QVariant AnalysisTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= rowCount()) return {};
  const Entry& entry = display_[static_cast<std::size_t>(index.row())];
  if (entry.is_break) return break_data(breaks_[entry.index], index.column(), role);
  const auto& r = rows_[entry.index];
  if (role == Qt::BackgroundRole) {
    const QColor c = row_color(color_by_, r, type_colors_, keys_, theme());
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

// A separator: one line of text in the first column, which the view spans
// over the others.
QVariant AnalysisTableModel::break_data(const processing::TimeBreak& b, int column, int role) const {
  switch (role) {
    case Qt::BackgroundRole:
      return QBrush(theme().header_bg);
    case Qt::TextAlignmentRole:
      return static_cast<int>(Qt::AlignCenter);
    case Qt::FontRole: {
      QFont font;
      font.setItalic(true);
      return font;
    }
    case Qt::DisplayRole: {
      if (column != 0) return {};
      const QString gap = tr("no analyses for %1").arg(qs(processing::gap_text(b.gap_seconds)));
      return several_spectrometers_ ? tr("%1: %2").arg(qs(rows_[b.above].mass_spectrometer), gap) : gap;
    }
    case Qt::ToolTipRole:
      return tr("%n run(s) above, %1 to %2 UTC", nullptr, static_cast<int>(b.session_runs))
          .arg(format_time(b.session_start), format_time(b.session_end));
    default:
      return {};
  }
}

Qt::ItemFlags AnalysisTableModel::flags(const QModelIndex& index) const {
  if (index.isValid() && is_break(index.row())) return Qt::NoItemFlags;
  return QAbstractTableModel::flags(index);
}

QVariant AnalysisTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* names[] = {"Run ID", "Type", "Sample", "Identifier", "Spec.", "Date (UTC)", "Extract", "Tag",
                                "Project", "Irradiation", "UUID"};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

}  // namespace pychron::ui
