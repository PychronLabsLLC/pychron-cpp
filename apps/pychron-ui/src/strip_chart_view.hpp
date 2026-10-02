#pragma once

// StripChartView: a thin QCustomPlot wrapper that draws a StripChartModel, one
// graph per detector in its colour. It owns no data: refresh() replaces every
// graph's points from the model's rings and applies the model's ranges.
//
// A detector that missed a row is a NaN point in the model, which QCustomPlot
// draws as a break in the line. On a log axis non-positive values are turned
// into NaN as well, and the axis is never handed a non-positive or non-finite
// range.

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "strip_chart_model.hpp"

class QCustomPlot;

namespace pychron::ui {

class StripChartView : public QWidget {
 public:
  static constexpr int kMinReplotMs = 50;  // repaint cap: 20 Hz

  // `model` must outlive the view.
  explicit StripChartView(StripChartModel& model, QWidget* parent = nullptr);

  // Pushes the model's data and ranges into the plot. The repaint itself is
  // capped at 20 Hz: one that comes too soon is deferred, not dropped.
  void refresh();

  int graph_count() const;
  int point_count(int graph) const;
  bool graph_visible(int graph) const;
  AxisRange shown_x() const;
  AxisRange shown_y() const;

 private:
  void apply_scale();
  void replot();

  StripChartModel& model_;
  QCustomPlot* plot_;
  YScale applied_scale_ = YScale::Linear;
  QElapsedTimer since_replot_;
  QTimer deferred_;  // single shot: the repaint a too-early refresh() asked for
};

}  // namespace pychron::ui
