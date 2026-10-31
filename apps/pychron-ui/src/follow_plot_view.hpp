#pragma once

// FollowPlotView: the plots of a followed run (FollowPlotModel), five strips
// stacked in one QCustomPlot that share a time axis: stage x, stage y, beam
// deviation, luminosity and grain size, each with what the finder saw and, on a
// simulated grain, what was true. It owns no data: refresh() replaces every
// graph's points from the model.
//
// A line with no value anywhere in the run (a real laser's true lines, a point
// grain's size) is hidden and left out of its strip's legend. A NaN in a line
// is a break. No axis is ever handed a non-finite or empty range.
//
// Colours come from plot_style(): the first line of a strip is palette slot 0,
// the second slot 1, on every strip. It follows PlotStyleHub::changed.

#include <array>

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "follow_plot_model.hpp"

class QCPAxisRect;
class QCPGraph;
class QCPLegend;
class QCustomPlot;

namespace pychron::ui {

class FollowPlotView : public QWidget {
  Q_OBJECT

 public:
  static constexpr int kMinReplotMs = 50;  // repaint cap: 20 Hz
  static constexpr int kStrips = 5;

  // `model` must outlive the view.
  explicit FollowPlotView(FollowPlotModel& model, QWidget* parent = nullptr);

  // Pushes the model's data and ranges into the plot. The repaint itself is
  // capped at 20 Hz: one that comes too soon is deferred, not dropped.
  void refresh();

  int strip_count() const { return kStrips; }
  int point_count(FollowLine line) const;
  bool line_visible(FollowLine line) const;

  // For tests.
  QCustomPlot* plot() const noexcept { return plot_; }

 private:
  void restyle();
  void replot();
  void set_all_x(double lo, double hi);
  void rebuild_legends();

  FollowPlotModel& model_;
  QCustomPlot* plot_;
  std::array<QCPAxisRect*, kStrips> rects_{};
  std::array<QCPLegend*, kStrips> legends_{};
  std::array<QCPGraph*, kFollowLines> graphs_{};
  bool setting_x_ = false;  // our own range change, not the user's
  bool auto_x_ = true;      // the x range follows the run until the user moves it
  double last_x_ = 0;
  unsigned shown_mask_ = 0;  // which lines the legends list
  QElapsedTimer since_replot_;
  QTimer deferred_;
};

}  // namespace pychron::ui
