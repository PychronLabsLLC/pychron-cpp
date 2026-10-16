#pragma once

// HistoryChart: one series of readings (a gauge's, a heater's) drawn over a
// chosen range that ends at the series' newest reading. It owns a copy of what
// it was last given and redraws it when the scale, the guides or the
// compactness change. A reading that is no finite number is a break in the line;
// on a log axis a reading that is not positive is a break too. Background and
// pen are plot_style()'s, set again on PlotStyleHub::changed.

#include <chrono>
#include <optional>
#include <vector>

#include <QDateTime>
#include <QWidget>

#include "reading_history.hpp"
#include "strip_chart_model.hpp"

class QCPItemStraightLine;
class QCustomPlot;
class QLabel;
class QStackedWidget;

namespace pychron::ui {

class HistoryChart : public QWidget {
 public:
  explicit HistoryChart(QWidget* parent = nullptr);

  // `newest_wall`: the wall time of t = 0, for the axis labels; invalid: the
  // axis is labelled in minutes before the newest reading instead.
  void set_series(const ReadingHistory::Series& readings, std::chrono::seconds range, const QDateTime& newest_wall);
  void set_log(bool log);
  void set_guides(std::vector<double> y);  // horizontal rules (alarm limits); drawn only when inside the view
  void set_compact(bool compact);          // the popup: no axis titles, fewer ticks

  struct Summary {
    int points = 0;
    double latest = 0, lowest = 0, highest = 0;
  };
  [[nodiscard]] std::optional<Summary> summary() const;  // nullopt with no reading in the range

  // For tests.
  [[nodiscard]] int point_count() const;    // drawn, breaks not counted
  [[nodiscard]] AxisRange shown_x() const;  // seconds since the epoch when given a wall time, else seconds before newest
  [[nodiscard]] AxisRange shown_y() const;
  [[nodiscard]] bool log() const { return log_; }
  [[nodiscard]] int guide_count() const { return static_cast<int>(guide_lines_.size()); }
  [[nodiscard]] QString x_label_at(double x) const;

 private:
  void apply_scale();
  void apply_axes();
  void redraw();
  [[nodiscard]] bool wall_axis() const { return newest_wall_.isValid(); }
  [[nodiscard]] double x_of(double t) const;

  QStackedWidget* stack_;
  QLabel* empty_;
  QCustomPlot* plot_;
  std::vector<QCPItemStraightLine*> guide_lines_;  // the plot's

  std::vector<TimeSeriesRing::Point> points_;  // within the range; t: seconds before the newest
  QString units_;
  std::chrono::seconds range_{600};
  QDateTime newest_wall_;
  std::vector<double> guides_;
  bool log_ = false;
  bool compact_ = false;
};

}  // namespace pychron::ui
