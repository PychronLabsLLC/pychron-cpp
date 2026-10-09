#include "intensities_model.hpp"
#include "theme.hpp"

#include <cmath>

#include <QBrush>

namespace pychron::ui {

namespace {
QString dash() { return QStringLiteral("—"); }
}  // namespace

IntensitiesModel::IntensitiesModel(std::vector<DetectorSeries> detectors, QObject* parent)
    : QAbstractTableModel(parent) {
  for (std::size_t i = 0; i < detectors.size(); ++i) {
    if (!detectors[i].color.isValid()) detectors[i].color = StripChartModel::palette_color(i);
    rows_.push_back(Row{std::move(detectors[i]), std::nullopt, {}});
  }
}

void IntensitiesModel::update(const spectrometer::IntensityReading& r) {
  for (auto& row : rows_) {
    auto it = r.reading.values.find(row.series.name);
    row.latest = it != r.reading.values.end() ? it->second : std::nullopt;
    if (!row.latest) continue;
    row.window.push_back(row.latest->mean);
    if (row.window.size() > static_cast<std::size_t>(kWindow) + 1) row.window.pop_front();
  }
  if (!rows_.empty())
    emit dataChanged(index(0, 0), index(static_cast<int>(rows_.size()) - 1, ColCount - 1));
}

void IntensitiesModel::set_isotope(const std::string& detector, const QString& isotope) {
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].series.name != detector) continue;
    rows_[i].series.isotope = isotope;
    const QModelIndex idx = index(static_cast<int>(i), ColIsotope);
    emit dataChanged(idx, idx);
  }
}

int IntensitiesModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int IntensitiesModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : ColCount;
}

double IntensitiesModel::sigma(const Row& row) {
  const auto n = static_cast<double>(row.window.size());
  double mean = 0.0;
  for (double v : row.window) mean += v;
  mean /= n;
  double ss = 0.0;
  for (double v : row.window) ss += (v - mean) * (v - mean);
  return std::sqrt(ss / n);
}

QVariant IntensitiesModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size()) ||
      index.column() < 0 || index.column() >= ColCount)
    return {};
  const Row& row = rows_[static_cast<std::size_t>(index.row())];
  const int col = index.column();
  const bool saturated = row.latest && row.latest->saturated;

  if (role == Qt::BackgroundRole) {
    if (col == ColColour) return QBrush(row.series.color);
    if (col == ColIntensity && saturated) return QBrush(theme().error);
    return {};
  }
  if (role == Qt::ToolTipRole) {
    if (col == ColIntensity && saturated) return QStringLiteral("saturated");
    return {};
  }
  if (role != Qt::DisplayRole) return {};

  switch (col) {
    case ColName: return QString::fromStdString(row.series.name);
    case ColIsotope: return row.series.isotope;
    case ColIntensity:
      return row.latest ? QString::number(row.latest->mean, 'f', 5) : dash();
    case ColSigma: return row.latest ? QString::number(sigma(row), 'f', 5) : dash();
    case ColUnits: return row.series.units;
    default: return {};
  }
}

QVariant IntensitiesModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  switch (section) {
    case ColColour: return QStringLiteral("Colour");
    case ColName: return QStringLiteral("Name");
    case ColIsotope: return QStringLiteral("Isotope");
    case ColIntensity: return QStringLiteral("Intensity");
    case ColSigma: return QStringLiteral("±1σ");
    case ColUnits: return QStringLiteral("Units");
    default: return {};
  }
}

}  // namespace pychron::ui
