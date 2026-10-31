#pragma once

// FollowPlotView: the plots of a followed run (FollowPlotModel). On the left
// the stage and the grain in the plane of the tray (stage mm, one scale on both
// axes) inside the fence the stage is held to. On the right three strips that
// share a time axis: beam deviation, luminosity and grain size, each with what
// the finder saw and, on a simulated grain, what was true. It owns no data:
// refresh() replaces every point from the model.
//
// A line with no value anywhere in the run (a real laser's true lines, a point
// grain's size) is hidden and left out of its strip's legend. A NaN in a line
// is a break in a strip; the plane's tracks join across it. No axis is ever
// handed a non-finite or empty range.
//
// Colours come from plot_style(): the first line of a strip is palette slot 0,
// the second slot 1, on every strip; in the plane stage 0, grain 1, fence 2. It
// follows PlotStyleHub::changed.

#include <array>

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "follow_plot_model.hpp"

class QCPAxisRect;
class QCPCurve;
class QCPGraph;
class QCPLegend;
class QCustomPlot;

namespace pychron::ui {

class FollowPlotView : public QWidget {
  Q_OBJECT

 public:
  static constexpr int kMinReplotMs = 50;  // repaint cap: 20 Hz
  static constexpr int kStrips = 3;
  static constexpr int kFencePoints = 73;  // closed

  // `model` must outlive the view.
  explicit FollowPlotView(FollowPlotModel& model, QWidget* parent = nullptr);

  // Pushes the model's data and ranges into the plot. The repaint itself is
  // capped at 20 Hz: one that comes too soon is deferred, not dropped.
  void refresh();

  int strip_count() const { return kStrips; }
  // Stage x and y are one track, grain x and y another.
  int point_count(FollowLine line) const;
  bool line_visible(FollowLine line) const;
  int fence_point_count() const;

  // For tests.
  QCustomPlot* plot() const noexcept { return plot_; }
  QCustomPlot* xy_plot() const noexcept { return xy_; }

 private:
  void restyle();
  void replot();
  void set_all_x(double lo, double hi);
  void rebuild_legends();
  void refresh_plane();
  void fit_plane();  // after the layout: one scale on both axes

  FollowPlotModel& model_;
  QCustomPlot* xy_;
  QCustomPlot* plot_;
  std::array<QCPAxisRect*, kStrips> rects_{};
  std::array<QCPLegend*, kStrips> legends_{};
  std::array<QCPGraph*, 2 * kStrips> graphs_{};  // from kFirstStripLine on
  QCPCurve* stage_track_ = nullptr;
  QCPCurve* grain_track_ = nullptr;
  QCPCurve* fence_ = nullptr;
  QCPGraph* stage_now_ = nullptr;
  QCPGraph* grain_now_ = nullptr;
  bool setting_x_ = false;  // our own range change, not the user's
  bool auto_x_ = true;      // the x range follows the run until the user moves it
  bool setting_plane_ = false;  // as setting_x_
  bool auto_plane_ = true;      // as auto_x_
  double fit_x_ = 0.5;          // what the plane must show: its center
  double fit_y_ = 0.5;
  double fit_width_ = 1;        // and size, mm
  double fit_height_ = 1;
  double last_x_ = 0;
  unsigned shown_mask_ = 0;  // which lines the legends list
  QElapsedTimer since_replot_;
  QTimer deferred_;
};

}  // namespace pychron::ui
