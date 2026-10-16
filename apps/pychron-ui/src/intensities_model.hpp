#pragma once

// IntensitiesModel: the table beside the strip chart, one row per detector,
// columns Color, Name, Isotope, Intensity, ±1σ, Units. The sigma is the
// population standard deviation of the previous kWindow means plus the current
// one; a row with no value shows an em dash and does not enter the window.
// Values are already gain-corrected. A detector whose line is not drawn in
// the chart (set_shown) is greyed; its values go on updating.

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <QAbstractTableModel>

#include "strip_chart_model.hpp"

namespace pychron::ui {

class IntensitiesModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { ColColour, ColName, ColIsotope, ColIntensity, ColSigma, ColUnits, ColCount };
  static constexpr int kWindow = 10;  // previous readings kept

  explicit IntensitiesModel(std::vector<DetectorSeries> detectors, QObject* parent = nullptr);

  void update(const spectrometer::IntensityReading& r);
  // Palette rows take the current plot_style()'s colours again (dataChanged
  // on the colour column). Called on PlotStyleHub::changed.
  void restyle();
  void set_isotope(const std::string& detector, const QString& isotope);
  // Whether the detector's line is drawn in the strip chart. A row that does
  // not exist is ignored (and is shown).
  void set_shown(int row, bool shown);
  bool shown(int row) const;

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;

 private:
  struct Row {
    DetectorSeries series;
    std::optional<spectrometer::Value> latest;
    std::deque<double> window;  // up to kWindow previous means plus the current
  };

  static double sigma(const Row& row);

  std::vector<Row> rows_;
};

}  // namespace pychron::ui
