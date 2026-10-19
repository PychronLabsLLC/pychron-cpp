#pragma once

// FieldTablePlot (field table editor spec, section 3): the field table
// editor's plot of one detector column, value against mass. Points are the
// rows that have a value on the detector; the fit curve is 200 samples over
// the detector's mass range widened by FieldTable::kInverseMargin on each
// side, and is left out for a discrete fit or a fit that fails. A point's
// tooltip gives its value and residual (value - fit).
//
// Colours from plot_style() / plot_series_color(0); restyles on
// PlotStyleHub::changed() (apps/pychron-ui AGENTS.md).

#include <string>
#include <utility>
#include <vector>

#include <QColor>
#include <QStringList>
#include <QWidget>

#include "pychron/systems/spectrometer/field_table.hpp"

class QCustomPlot;
class QMouseEvent;

namespace pychron::ui {

class FieldTablePlot : public QWidget {
  Q_OBJECT

 public:
  static constexpr int kCurveSamples = 200;

  explicit FieldTablePlot(QWidget* parent = nullptr);

  // Draws `detector` of `table` (a copy is kept for a restyle).
  void show_detector(const spectrometer::FieldTable& table, const std::string& detector);
  const std::string& detector() const noexcept { return detector_; }

  // What the last redraw drew.
  int point_count() const noexcept { return static_cast<int>(tips_.size()); }
  int curve_sample_count() const noexcept { return curve_samples_; }
  std::pair<double, double> curve_mass_range() const noexcept { return curve_range_; }
  // "Ar40: value 5.00100, residual 0.00012" (no residual without a fit).
  const QStringList& point_tooltips() const noexcept { return tips_; }
  QColor point_color() const;
  QColor curve_color() const;

 private:
  void redraw();
  void on_mouse_move(QMouseEvent* event);

  QCustomPlot* plot_;
  spectrometer::FieldTable table_;
  std::string detector_;
  std::vector<double> masses_, values_;  // the points, in table order
  QStringList tips_;
  int curve_samples_ = 0;
  std::pair<double, double> curve_range_{0, 0};
};

}  // namespace pychron::ui
